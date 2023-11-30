/*
   Unix SMB/CIFS implementation.
   Locking functions
   Copyright (C) Andrew Tridgell 1992-2000
   Copyright (C) Jeremy Allison 1992-2006
   Copyright (C) Volker Lendecke 2005

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

   Revision History:

   12 aug 96: Erik.Devriendt@te6.siemens.be
   added support for shared memory implementation of share mode locking

   May 1997. Jeremy Allison (jallison@whistle.com). Modified share mode
   locking to deal with multiple share modes per open file.

   September 1997. Jeremy Allison (jallison@whistle.com). Added oplock
   support.

   rewritten completely to use new tdb code. Tridge, Dec '99

   Added POSIX locking support. Jeremy Allison (jeremy@valinux.com), Apr. 2000.
   Added Unix Extensions POSIX locking support. Jeremy Allison Mar 2006.
*/

#include "includes.h"
#include "system/filesys.h"
#include "lib/util/server_id.h"
#include "locking/proto.h"
#include "smbd/globals.h"
#include "dbwrap/dbwrap.h"
#include "dbwrap/dbwrap_open.h"
#include "../libcli/security/security.h"
#include "serverid.h"
#include "messages.h"
#include "util_tdb.h"
#include "../librpc/gen_ndr/ndr_open_files.h"
#include "source3/lib/dbwrap/dbwrap_watch.h"
#include "locking/leases_db.h"
#include "../lib/util/memcache.h"
#include "lib/util/tevent_ntstatus.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_LOCKING

#define NO_LOCKING_COUNT (-1)

/* the locking database handle */
static struct db_context *lock_db;

static bool locking_init_internal(bool read_only)
{
	struct db_context *backend;
	char *db_path;

	brl_init(read_only);

	if (lock_db)
		return True;

	db_path = lock_path(talloc_tos(), "locking.tdb");
	if (db_path == NULL) {
		return false;
	}

	backend = db_open(NULL, db_path,
			  SMB_OPEN_DATABASE_TDB_HASH_SIZE,
			  TDB_DEFAULT|
			  TDB_VOLATILE|
			  TDB_CLEAR_IF_FIRST|
			  TDB_INCOMPATIBLE_HASH|
			  TDB_SEQNUM,
			  read_only?O_RDONLY:O_RDWR|O_CREAT, 0644,
			  DBWRAP_LOCK_ORDER_1, DBWRAP_FLAG_NONE);
	TALLOC_FREE(db_path);
	if (!backend) {
		DEBUG(0,("ERROR: Failed to initialise locking database\n"));
		return False;
	}

	lock_db = db_open_watched(NULL, &backend, global_messaging_context());
	if (lock_db == NULL) {
		DBG_ERR("db_open_watched failed\n");
		TALLOC_FREE(backend);
		return false;
	}

	if (!posix_locking_init(read_only)) {
		TALLOC_FREE(lock_db);
		return False;
	}

	return True;
}

bool locking_init(void)
{
	return locking_init_internal(false);
}

bool locking_init_readonly(void)
{
	return locking_init_internal(true);
}

/*******************************************************************
 Deinitialize the share_mode management.
******************************************************************/

bool locking_end(void)
{
	brl_shutdown();
	TALLOC_FREE(lock_db);
	return true;
}

/*******************************************************************
 Form a static locking key for a dev/inode pair.
******************************************************************/

static TDB_DATA locking_key(const struct file_id *id)
{
	return make_tdb_data((const uint8_t *)id, sizeof(*id));
}

/*******************************************************************
 Share mode cache utility functions that store/delete/retrieve
 entries from memcache.

 For now share the statcache (global cache) memory space. If
 a lock record gets orphaned (which shouldn't happen as we're
 using the same locking_key data as lookup) it will eventually
 fall out of the cache via the normal LRU trim mechanism. If
 necessary we can always make this a separate (smaller) cache.
******************************************************************/

static DATA_BLOB memcache_key(const struct file_id *id)
{
	return data_blob_const((const void *)id, sizeof(*id));
}

static void share_mode_memcache_store(struct share_mode_data *d)
{
	const DATA_BLOB key = memcache_key(&d->id);

	DBG_DEBUG("stored entry for file %s seq %"PRIx64" key %s\n",
		  d->base_name,
		  d->sequence_number,
		  file_id_string(talloc_tos(), &d->id));

	/* Ensure everything stored in the cache is pristine. */
	d->modified = false;
	d->fresh = false;

	/*
	 * Ensure the memory going into the cache
	 * doesn't have a destructor so it can be
	 * cleanly evicted by the memcache LRU
	 * mechanism.
	 */
	talloc_set_destructor(d, NULL);

	/* Cache will own d after this call. */
	memcache_add_talloc(NULL,
			SHARE_MODE_LOCK_CACHE,
			key,
			&d);
}

/*
 * NB. We use ndr_pull_hyper on a stack-created
 * struct ndr_pull with no talloc allowed, as we
 * need this to be really fast as an ndr-peek into
 * the first 9 bytes of the blob.
 */

static enum ndr_err_code get_share_mode_blob_header(
	DATA_BLOB *blob, uint64_t *pseq, uint8_t *pflags)
{
	struct ndr_pull ndr = {.data = blob->data, .data_size = blob->length};
	NDR_CHECK(ndr_pull_hyper(&ndr, NDR_SCALARS, pseq));
	NDR_CHECK(ndr_pull_uint8(&ndr, NDR_SCALARS, pflags));
	return NDR_ERR_SUCCESS;
}

struct fsp_update_share_mode_flags_state {
	enum ndr_err_code ndr_err;
	uint8_t share_mode_flags;
};

static void fsp_update_share_mode_flags_fn(
	struct db_record *rec, bool *modified_dependent, void *private_data)
{
	struct fsp_update_share_mode_flags_state *state = private_data;
	TDB_DATA value = dbwrap_record_get_value(rec);
	DATA_BLOB blob = { .data = value.dptr, .length = value.dsize };
	uint64_t seq;

	state->ndr_err = get_share_mode_blob_header(
		&blob, &seq, &state->share_mode_flags);
}

static NTSTATUS fsp_update_share_mode_flags(struct files_struct *fsp)
{
	struct fsp_update_share_mode_flags_state state = {0};
	int seqnum = dbwrap_get_seqnum(lock_db);
	NTSTATUS status;

	if (seqnum == fsp->share_mode_flags_seqnum) {
		return NT_STATUS_OK;
	}

	status = share_mode_do_locked(
		fsp->file_id, fsp_update_share_mode_flags_fn, &state);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_DEBUG("share_mode_do_locked returned %s\n",
			  nt_errstr(status));
		return status;
	}

	if (!NDR_ERR_CODE_IS_SUCCESS(state.ndr_err)) {
		DBG_DEBUG("get_share_mode_blob_header returned %s\n",
			  ndr_errstr(state.ndr_err));
		return ndr_map_error2ntstatus(state.ndr_err);
	}

	fsp->share_mode_flags_seqnum = seqnum;
	fsp->share_mode_flags = state.share_mode_flags;

	return NT_STATUS_OK;
}

bool file_has_read_lease(struct files_struct *fsp)
{
	NTSTATUS status;

	status = fsp_update_share_mode_flags(fsp);
	if (!NT_STATUS_IS_OK(status)) {
		/* Safe default for leases */
		return true;
	}

	return (fsp->share_mode_flags & SHARE_MODE_HAS_READ_LEASE) != 0;
}

static int share_mode_data_nofree_destructor(struct share_mode_data *d)
{
	return -1;
}

static struct share_mode_data *share_mode_memcache_fetch(TALLOC_CTX *mem_ctx,
					const TDB_DATA id_key,
					DATA_BLOB *blob)
{
	enum ndr_err_code ndr_err;
	struct share_mode_data *d;
	uint64_t sequence_number;
	uint8_t flags;
	void *ptr;
	struct file_id id;
	DATA_BLOB key;

	/* Ensure this is a locking_key record. */
	if (id_key.dsize != sizeof(id)) {
		return NULL;
	}

	memcpy(&id, id_key.dptr, id_key.dsize);
	key = memcache_key(&id);

	ptr = memcache_lookup_talloc(NULL,
			SHARE_MODE_LOCK_CACHE,
			key);
	if (ptr == NULL) {
		DEBUG(10,("failed to find entry for key %s\n",
			file_id_string(mem_ctx, &id)));
		return NULL;
	}
	/* sequence number key is at start of blob. */
	ndr_err = get_share_mode_blob_header(blob, &sequence_number, &flags);
	if (ndr_err != NDR_ERR_SUCCESS) {
		/* Bad blob. Remove entry. */
		DEBUG(10,("bad blob %u key %s\n",
			(unsigned int)ndr_err,
			file_id_string(mem_ctx, &id)));
		memcache_delete(NULL,
			SHARE_MODE_LOCK_CACHE,
			key);
		return NULL;
	}

	d = (struct share_mode_data *)ptr;
	if (d->sequence_number != sequence_number) {
		DBG_DEBUG("seq changed (cached %"PRIx64") (new %"PRIx64") "
			  "for key %s\n",
			  d->sequence_number,
			  sequence_number,
			  file_id_string(mem_ctx, &id));
		/* Cache out of date. Remove entry. */
		memcache_delete(NULL,
			SHARE_MODE_LOCK_CACHE,
			key);
		return NULL;
	}

	/* Move onto mem_ctx. */
	d = talloc_move(mem_ctx, &ptr);

	/*
	 * Now we own d, prevent the cache from freeing it
	 * when we delete the entry.
	 */
	talloc_set_destructor(d, share_mode_data_nofree_destructor);

	/* Remove from the cache. We own it now. */
	memcache_delete(NULL,
			SHARE_MODE_LOCK_CACHE,
			key);

	/* And reset the destructor to none. */
	talloc_set_destructor(d, NULL);

	DBG_DEBUG("fetched entry for file %s seq %"PRIx64" key %s\n",
		  d->base_name,
		  d->sequence_number,
		  file_id_string(mem_ctx, &id));

	return d;
}

/*******************************************************************
 Get all share mode entries for a dev/inode pair.
********************************************************************/

static struct share_mode_data *parse_share_modes(TALLOC_CTX *mem_ctx,
						const TDB_DATA key,
						const TDB_DATA dbuf)
{
	struct share_mode_data *d;
	enum ndr_err_code ndr_err;
	DATA_BLOB blob;

	blob.data = dbuf.dptr;
	blob.length = dbuf.dsize;

	/* See if we already have a cached copy of this key. */
	d = share_mode_memcache_fetch(mem_ctx, key, &blob);
	if (d != NULL) {
		return d;
	}

	d = talloc(mem_ctx, struct share_mode_data);
	if (d == NULL) {
		DEBUG(0, ("talloc failed\n"));
		goto fail;
	}

	ndr_err = ndr_pull_struct_blob_all(
		&blob, d, d, (ndr_pull_flags_fn_t)ndr_pull_share_mode_data);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		DBG_WARNING("ndr_pull_share_mode_data failed: %s\n",
			    ndr_errstr(ndr_err));
		goto fail;
	}

	if (DEBUGLEVEL >= 10) {
		DEBUG(10, ("parse_share_modes:\n"));
		NDR_PRINT_DEBUG(share_mode_data, d);
	}

	return d;
fail:
	TALLOC_FREE(d);
	return NULL;
}

/*******************************************************************
 If modified, store the share_mode_data back into the database.
********************************************************************/

static NTSTATUS share_mode_data_store(struct share_mode_data *d)
{
	DATA_BLOB blob;
	enum ndr_err_code ndr_err;
	NTSTATUS status;

	if (!d->modified) {
		DBG_DEBUG("not modified\n");
		return NT_STATUS_OK;
	}

	if (DEBUGLEVEL >= 10) {
		DBG_DEBUG("\n");
		NDR_PRINT_DEBUG(share_mode_data, d);
	}

	d->sequence_number += 1;
	remove_stale_share_mode_entries(d);

	if (d->num_share_modes == 0) {
		if (d->fresh) {
			DBG_DEBUG("Ignoring fresh emtpy record\n");
			return NT_STATUS_OK;
		}
		status = dbwrap_record_delete(d->record);
		return status;
	}

	ndr_err = ndr_push_struct_blob(
		&blob, d, d, (ndr_push_flags_fn_t)ndr_push_share_mode_data);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		DBG_DEBUG("ndr_push_share_mode_data failed: %s\n",
			  ndr_errstr(ndr_err));
		return ndr_map_error2ntstatus(ndr_err);
	}

	status = dbwrap_record_store(
		d->record,
		(TDB_DATA) { .dptr = blob.data, .dsize = blob.length },
		TDB_REPLACE);
	TALLOC_FREE(blob.data);

	if (!NT_STATUS_IS_OK(status)) {
		DBG_DEBUG("dbwrap_record_store failed: %s\n",
			  nt_errstr(status));
	}

	return status;
}

/*******************************************************************
 Allocate a new share_mode_data struct, mark it unmodified.
 fresh is set to note that currently there is no database entry.
********************************************************************/

static struct share_mode_data *fresh_share_mode_lock(
	TALLOC_CTX *mem_ctx, const char *servicepath,
	const struct smb_filename *smb_fname,
	const struct timespec *old_write_time)
{
	struct share_mode_data *d;

	if ((servicepath == NULL) || (smb_fname == NULL) ||
	    (old_write_time == NULL)) {
		return NULL;
	}

	d = talloc_zero(mem_ctx, struct share_mode_data);
	if (d == NULL) {
		goto fail;
	}
	/* New record - new sequence number. */
	generate_random_buffer((uint8_t *)&d->sequence_number, 8);

	d->base_name = talloc_strdup(d, smb_fname->base_name);
	if (d->base_name == NULL) {
		goto fail;
	}
	if (smb_fname->stream_name != NULL) {
		d->stream_name = talloc_strdup(d, smb_fname->stream_name);
		if (d->stream_name == NULL) {
			goto fail;
		}
	}
	d->servicepath = talloc_strdup(d, servicepath);
	if (d->servicepath == NULL) {
		goto fail;
	}
	d->old_write_time = *old_write_time;
	d->modified = false;
	d->fresh = true;
	return d;
fail:
	DEBUG(0, ("talloc failed\n"));
	TALLOC_FREE(d);
	return NULL;
}

/*
 * We can only ever have one share mode locked. Use a static
 * share_mode_data pointer that is shared by multiple nested
 * share_mode_lock structures, explicitly refcounted.
 */
static struct share_mode_data *static_share_mode_data = NULL;
static size_t static_share_mode_data_refcount = 0;

/*
 * db_record for the above. With dbwrap_do_locked we can get a
 * db_record on the stack, which we can't TALLOC_FREE but which we
 * need to share with a nested get_share_mode_lock call.
 */
static struct db_record *static_share_mode_record = NULL;
static bool static_share_mode_record_talloced = false;

/*******************************************************************
 Either fetch a share mode from the database, or allocate a fresh
 one if the record doesn't exist.
********************************************************************/

static NTSTATUS get_static_share_mode_data(
	struct db_record *rec,
	struct file_id id,
	const char *servicepath,
	const struct smb_filename *smb_fname,
	const struct timespec *old_write_time)
{
	struct share_mode_data *d;
	TDB_DATA value = dbwrap_record_get_value(rec);

	SMB_ASSERT(static_share_mode_data == NULL);

	if (value.dptr == NULL) {
		d = fresh_share_mode_lock(
			lock_db, servicepath, smb_fname, old_write_time);
		if (d == NULL) {
			return NT_STATUS_NO_MEMORY;
		}
	} else {
		TDB_DATA key = locking_key(&id);
		d = parse_share_modes(lock_db, key, value);
		if (d == NULL) {
			return NT_STATUS_INTERNAL_DB_CORRUPTION;
		}
	}

	d->id = id;
	d->record = rec;

	static_share_mode_data = d;

	return NT_STATUS_OK;
}

/*******************************************************************
 Get a share_mode_lock, Reference counted to allow nested calls.
********************************************************************/

static int share_mode_lock_destructor(struct share_mode_lock *lck);

struct share_mode_lock *get_share_mode_lock(
	TALLOC_CTX *mem_ctx,
	struct file_id id,
	const char *servicepath,
	const struct smb_filename *smb_fname,
	const struct timespec *old_write_time)
{
	TDB_DATA key = locking_key(&id);
	struct share_mode_lock *lck = NULL;
	NTSTATUS status;

	lck = talloc(mem_ctx, struct share_mode_lock);
	if (lck == NULL) {
		DEBUG(1, ("talloc failed\n"));
		return NULL;
	}

	if (static_share_mode_data != NULL) {
		if (!file_id_equal(&static_share_mode_data->id, &id)) {
			DEBUG(1, ("Can not lock two share modes "
				  "simultaneously\n"));
			goto fail;
		}
		goto done;
	}

	SMB_ASSERT(static_share_mode_data_refcount == 0);

	if (static_share_mode_record == NULL) {
		static_share_mode_record = dbwrap_fetch_locked(
			lock_db, lock_db, key);
		if (static_share_mode_record == NULL) {
			DEBUG(3, ("Could not lock share entry\n"));
			goto fail;
		}
		static_share_mode_record_talloced = true;

		status = get_static_share_mode_data(
			static_share_mode_record,
			id,
			servicepath,
			smb_fname,
			old_write_time);
		if (!NT_STATUS_IS_OK(status)) {
			DBG_DEBUG("get_static_share_mode_data failed: %s\n",
				  nt_errstr(status));
			TALLOC_FREE(static_share_mode_record);
			goto fail;
		}
	} else {
		TDB_DATA static_key;
		int cmp;

		static_key = dbwrap_record_get_key(static_share_mode_record);

		cmp = tdb_data_cmp(static_key, key);
		if (cmp != 0) {
			DBG_WARNING("Can not lock two share modes "
				    "simultaneously\n");
			return NULL;
		}

		status = get_static_share_mode_data(
			static_share_mode_record,
			id,
			servicepath,
			smb_fname,
			old_write_time);
		if (!NT_STATUS_IS_OK(status)) {
			DBG_WARNING("get_static_share_mode_data failed: %s\n",
				    nt_errstr(status));
			goto fail;
		}
	}

done:
	static_share_mode_data_refcount += 1;
	lck->data = static_share_mode_data;

	talloc_set_destructor(lck, share_mode_lock_destructor);

	return lck;
fail:
	TALLOC_FREE(lck);
	return NULL;
}

static int share_mode_lock_destructor(struct share_mode_lock *lck)
{
	NTSTATUS status;

	SMB_ASSERT(static_share_mode_data_refcount > 0);
	static_share_mode_data_refcount -= 1;

	if (static_share_mode_data_refcount > 0) {
		return 0;
	}

	status = share_mode_data_store(static_share_mode_data);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_ERR("share_mode_data_store failed: %s\n",
			nt_errstr(status));
		smb_panic("Could not store share mode data\n");
	}

	/*
	 * Drop the locking.tdb lock before moving the share_mode_data
	 * to memcache
	 */
	SMB_ASSERT(static_share_mode_data->record == static_share_mode_record);
	static_share_mode_data->record = NULL;

	if (static_share_mode_record_talloced) {
		TALLOC_FREE(static_share_mode_record);
	}

	if (static_share_mode_data->num_share_modes != 0) {
		/*
		 * This is worth keeping. Without share modes,
		 * share_mode_data_store above has left nothing in the
		 * database.
		 */
		share_mode_memcache_store(static_share_mode_data);
		static_share_mode_data = NULL;
	} else {
		/*
		 * The next opener of this file will find an empty
		 * locking.tdb record. Don't store the share_mode_data
		 * in the memcache, fresh_share_mode_lock() will
		 * generate a fresh seqnum anyway, obsoleting the
		 * cache entry.
		 */
		TALLOC_FREE(static_share_mode_data);
	}

	return 0;
}

struct share_mode_do_locked_state {
	void (*fn)(struct db_record *rec,
		   bool *modified_dependent,
		   void *private_data);
	void *private_data;
};

static void share_mode_do_locked_fn(struct db_record *rec,
				    void *private_data)
{
	struct share_mode_do_locked_state *state = private_data;
	bool modified_dependent = false;
	bool reset_static_share_mode_record = false;

	if (static_share_mode_record == NULL) {
		static_share_mode_record = rec;
		static_share_mode_record_talloced = false;
		reset_static_share_mode_record = true;
	} else {
		SMB_ASSERT(static_share_mode_record == rec);
	}

	state->fn(rec, &modified_dependent, state->private_data);

	if (modified_dependent) {
		dbwrap_watched_wakeup(rec);
	}

	if (reset_static_share_mode_record) {
		static_share_mode_record = NULL;
	}
}

NTSTATUS share_mode_do_locked(
	struct file_id id,
	void (*fn)(struct db_record *rec,
		   bool *modified_dependent,
		   void *private_data),
	void *private_data)
{
	TDB_DATA key = locking_key(&id);
	size_t refcount = static_share_mode_data_refcount;

	if (static_share_mode_record != NULL) {
		bool modified_dependent = false;
		TDB_DATA static_key;
		int cmp;

		static_key = dbwrap_record_get_key(static_share_mode_record);

		cmp = tdb_data_cmp(static_key, key);
		if (cmp != 0) {
			DBG_WARNING("Can not lock two share modes "
				    "simultaneously\n");
			return NT_STATUS_INVALID_LOCK_SEQUENCE;
		}

		fn(static_share_mode_record,
		   &modified_dependent,
		   private_data);

		if (modified_dependent) {
			dbwrap_watched_wakeup(static_share_mode_record);
		}
	} else {
		struct share_mode_do_locked_state state = {
			.fn = fn, .private_data = private_data,
		};
		NTSTATUS status;

		status = dbwrap_do_locked(
			lock_db, key, share_mode_do_locked_fn, &state);
		if (!NT_STATUS_IS_OK(status)) {
			DBG_WARNING("dbwrap_do_locked failed: %s\n",
				    nt_errstr(status));
			return status;
		}
	}

	SMB_ASSERT(refcount == static_share_mode_data_refcount);

	return NT_STATUS_OK;
}

static void share_mode_wakeup_waiters_fn(struct db_record *rec,
					 bool *modified_dependent,
					 void *private_data)
{
	*modified_dependent = true;
}

NTSTATUS share_mode_wakeup_waiters(struct file_id id)
{
	return share_mode_do_locked(id, share_mode_wakeup_waiters_fn, NULL);
}

struct fetch_share_mode_unlocked_state {
	TALLOC_CTX *mem_ctx;
	struct share_mode_lock *lck;
};

static void fetch_share_mode_unlocked_parser(
	TDB_DATA key, TDB_DATA data, void *private_data)
{
	struct fetch_share_mode_unlocked_state *state = private_data;

	if (data.dsize == 0) {
		/* Likely a ctdb tombstone record, ignore it */
		return;
	}

	state->lck = talloc(state->mem_ctx, struct share_mode_lock);
	if (state->lck == NULL) {
		DEBUG(0, ("talloc failed\n"));
		return;
	}

	state->lck->data = parse_share_modes(state->lck, key, data);
}

/*******************************************************************
 Get a share_mode_lock without locking the database or reference
 counting. Used by smbstatus to display existing share modes.
********************************************************************/

struct share_mode_lock *fetch_share_mode_unlocked(TALLOC_CTX *mem_ctx,
						  struct file_id id)
{
	struct fetch_share_mode_unlocked_state state = { .mem_ctx = mem_ctx };
	TDB_DATA key = locking_key(&id);
	NTSTATUS status;

	status = dbwrap_parse_record(
		lock_db, key, fetch_share_mode_unlocked_parser, &state);
	if (!NT_STATUS_IS_OK(status)) {
		return NULL;
	}
	return state.lck;
}

static void fetch_share_mode_done(struct tevent_req *subreq);

struct fetch_share_mode_state {
	struct file_id id;
	TDB_DATA key;
	struct fetch_share_mode_unlocked_state parser_state;
	enum dbwrap_req_state req_state;
};

/**
 * @brief Get a share_mode_lock without locking or refcounting
 *
 * This can be used in a clustered Samba environment where the async dbwrap
 * request is sent over a socket to the local ctdbd. If the send queue is full
 * and the caller was issuing multiple async dbwrap requests in a loop, the
 * caller knows it's probably time to stop sending requests for now and try
 * again later.
 *
 * @param[in]  mem_ctx The talloc memory context to use.
 *
 * @param[in]  ev      The event context to work on.
 *
 * @param[in]  id      The file id for the locking.tdb key
 *
 * @param[out] queued  This boolean out parameter tells the caller whether the
 *                     async request is blocked in a full send queue:
 *
 *                     false := request is dispatched
 *
 *                     true  := send queue is full, request waiting to be
 *                              dispatched
 *
 * @return             The new async request, NULL on error.
 **/
struct tevent_req *fetch_share_mode_send(TALLOC_CTX *mem_ctx,
					 struct tevent_context *ev,
					 struct file_id id,
					 bool *queued)
{
	struct tevent_req *req = NULL;
	struct fetch_share_mode_state *state = NULL;
	struct tevent_req *subreq = NULL;

	*queued = false;

	req = tevent_req_create(mem_ctx, &state,
				struct fetch_share_mode_state);
	if (req == NULL) {
		return NULL;
	}

	state->id = id;
	state->key = locking_key(&state->id);
	state->parser_state.mem_ctx = state;

	subreq = dbwrap_parse_record_send(state,
					  ev,
					  lock_db,
					  state->key,
					  fetch_share_mode_unlocked_parser,
					  &state->parser_state,
					  &state->req_state);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, fetch_share_mode_done, req);

	if (state->req_state < DBWRAP_REQ_DISPATCHED) {
		*queued = true;
	}
	return req;
}

static void fetch_share_mode_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	NTSTATUS status;

	status = dbwrap_parse_record_recv(subreq);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	tevent_req_done(req);
	return;
}

NTSTATUS fetch_share_mode_recv(struct tevent_req *req,
			       TALLOC_CTX *mem_ctx,
			       struct share_mode_lock **_lck)
{
	struct fetch_share_mode_state *state = tevent_req_data(
		req, struct fetch_share_mode_state);
	struct share_mode_lock *lck = NULL;

	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return status;
	}

	if (state->parser_state.lck->data == NULL) {
		tevent_req_received(req);
		return NT_STATUS_NOT_FOUND;
	}

	lck = talloc_move(mem_ctx, &state->parser_state.lck);

	if (DEBUGLEVEL >= 10) {
		DBG_DEBUG("share_mode_data:\n");
		NDR_PRINT_DEBUG(share_mode_data, lck->data);
	}

	*_lck = lck;
	tevent_req_received(req);
	return NT_STATUS_OK;
}

struct share_mode_forall_state {
	int (*fn)(struct file_id fid, const struct share_mode_data *data,
		  void *private_data);
	void *private_data;
};

static int share_mode_traverse_fn(struct db_record *rec, void *_state)
{
	struct share_mode_forall_state *state =
		(struct share_mode_forall_state *)_state;
	TDB_DATA key;
	TDB_DATA value;
	DATA_BLOB blob;
	enum ndr_err_code ndr_err;
	struct share_mode_data *d;
	struct file_id fid;
	int ret;

	key = dbwrap_record_get_key(rec);
	value = dbwrap_record_get_value(rec);

	/* Ensure this is a locking_key record. */
	if (key.dsize != sizeof(fid)) {
		return 0;
	}
	memcpy(&fid, key.dptr, sizeof(fid));

	d = talloc(talloc_tos(), struct share_mode_data);
	if (d == NULL) {
		return 0;
	}

	blob.data = value.dptr;
	blob.length = value.dsize;

	ndr_err = ndr_pull_struct_blob_all(
		&blob, d, d, (ndr_pull_flags_fn_t)ndr_pull_share_mode_data);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		DEBUG(1, ("ndr_pull_share_mode_lock failed\n"));
		return 0;
	}

	if (DEBUGLEVEL > 10) {
		DEBUG(11, ("parse_share_modes:\n"));
		NDR_PRINT_DEBUG(share_mode_data, d);
	}

	ret = state->fn(fid, d, state->private_data);

	TALLOC_FREE(d);
	return ret;
}

int share_mode_forall(int (*fn)(struct file_id fid,
				const struct share_mode_data *data,
				void *private_data),
		      void *private_data)
{
	struct share_mode_forall_state state = {
		.fn = fn,
		.private_data = private_data
	};
	NTSTATUS status;
	int count;

	if (lock_db == NULL) {
		return 0;
	}

	status = dbwrap_traverse_read(lock_db, share_mode_traverse_fn,
				      &state, &count);
	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}

	return count;
}

struct share_entry_forall_state {
	int (*fn)(struct file_id fid,
		  const struct share_mode_data *data,
		  const struct share_mode_entry *entry,
		  void *private_data);
	void *private_data;
};

static int share_entry_traverse_fn(struct file_id fid,
				   const struct share_mode_data *data,
				   void *private_data)
{
	struct share_entry_forall_state *state = private_data;
	uint32_t i;

	for (i=0; i<data->num_share_modes; i++) {
		int ret;

		ret = state->fn(fid,
				data,
				&data->share_modes[i],
				state->private_data);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

/*******************************************************************
 Call the specified function on each entry under management by the
 share mode system.
********************************************************************/

int share_entry_forall(int (*fn)(struct file_id fid,
				 const struct share_mode_data *data,
				 const struct share_mode_entry *entry,
				 void *private_data),
		      void *private_data)
{
	struct share_entry_forall_state state = {
		.fn = fn, .private_data = private_data };

	return share_mode_forall(share_entry_traverse_fn, &state);
}

static bool cleanup_disconnected_lease(struct share_mode_lock *lck,
				       struct share_mode_entry *e,
				       void *private_data)
{
	struct share_mode_data *d = lck->data;
	NTSTATUS status;

	status = leases_db_del(&e->client_guid, &e->lease_key, &d->id);

	if (!NT_STATUS_IS_OK(status)) {
		DBG_DEBUG("leases_db_del failed: %s\n",
			  nt_errstr(status));
	}

	return false;
}

bool share_mode_cleanup_disconnected(struct file_id fid,
				     uint64_t open_persistent_id)
{
	bool ret = false;
	TALLOC_CTX *frame = talloc_stackframe();
	unsigned n;
	struct share_mode_data *data;
	struct share_mode_lock *lck;
	bool ok;

	lck = get_existing_share_mode_lock(frame, fid);
	if (lck == NULL) {
		DEBUG(5, ("share_mode_cleanup_disconnected: "
			  "Could not fetch share mode entry for %s\n",
			  file_id_string(frame, &fid)));
		goto done;
	}
	data = lck->data;

	for (n=0; n < data->num_share_modes; n++) {
		struct share_mode_entry *entry = &data->share_modes[n];

		if (!server_id_is_disconnected(&entry->pid)) {
			struct server_id_buf tmp;
			DEBUG(5, ("share_mode_cleanup_disconnected: "
				  "file (file-id='%s', servicepath='%s', "
				  "base_name='%s%s%s') "
				  "is used by server %s ==> do not cleanup\n",
				  file_id_string(frame, &fid),
				  data->servicepath,
				  data->base_name,
				  (data->stream_name == NULL)
				  ? "" : "', stream_name='",
				  (data->stream_name == NULL)
				  ? "" : data->stream_name,
				  server_id_str_buf(entry->pid, &tmp)));
			goto done;
		}
		if (open_persistent_id != entry->share_file_id) {
			DBG_INFO("entry for file "
				 "(file-id='%s', servicepath='%s', "
				 "base_name='%s%s%s') "
				 "has share_file_id %"PRIu64" but expected "
				 "%"PRIu64"==> do not cleanup\n",
				 file_id_string(frame, &fid),
				 data->servicepath,
				 data->base_name,
				 (data->stream_name == NULL)
				 ? "" : "', stream_name='",
				 (data->stream_name == NULL)
				 ? "" : data->stream_name,
				 entry->share_file_id,
				 open_persistent_id);
			goto done;
		}
	}

	ok = share_mode_forall_leases(lck, cleanup_disconnected_lease, NULL);
	if (!ok) {
		DBG_DEBUG("failed to clean up leases associated "
			  "with file (file-id='%s', servicepath='%s', "
			  "base_name='%s%s%s') and open_persistent_id %"PRIu64" "
			  "==> do not cleanup\n",
			  file_id_string(frame, &fid),
			  data->servicepath,
			  data->base_name,
			  (data->stream_name == NULL)
			  ? "" : "', stream_name='",
			  (data->stream_name == NULL)
			  ? "" : data->stream_name,
			  open_persistent_id);
	}

	ok = brl_cleanup_disconnected(fid, open_persistent_id);
	if (!ok) {
		DBG_DEBUG("failed to clean up byte range locks associated "
			  "with file (file-id='%s', servicepath='%s', "
			  "base_name='%s%s%s') and open_persistent_id %"PRIu64" "
			  "==> do not cleanup\n",
			  file_id_string(frame, &fid),
			  data->servicepath,
			  data->base_name,
			  (data->stream_name == NULL)
			  ? "" : "', stream_name='",
			  (data->stream_name == NULL)
			  ? "" : data->stream_name,
			  open_persistent_id);
		goto done;
	}

	DBG_DEBUG("cleaning up %u entries for file "
		  "(file-id='%s', servicepath='%s', "
		  "base_name='%s%s%s') "
		  "from open_persistent_id %"PRIu64"\n",
		  data->num_share_modes,
		  file_id_string(frame, &fid),
		  data->servicepath,
		  data->base_name,
		  (data->stream_name == NULL)
		  ? "" : "', stream_name='",
		  (data->stream_name == NULL)
		  ? "" : data->stream_name,
		  open_persistent_id);

	data->num_share_modes = 0;
	data->modified = true;

	ret = true;
done:
	talloc_free(frame);
	return ret;
}
