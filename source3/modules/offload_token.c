/*
   Unix SMB/CIFS implementation.

   Copyright (C) Ralph Boehme 2017

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
*/

#include "includes.h"
#include "smbd/smbd.h"
#include "smbd/globals.h"
#include "dbwrap/dbwrap.h"
#include "dbwrap/dbwrap_rbt.h"
#include "dbwrap/dbwrap_open.h"
#include "../lib/util/util_tdb.h"
#include "librpc/gen_ndr/ndr_ioctl.h"
#include "librpc/gen_ndr/ioctl.h"
#include "offload_token.h"

struct vfs_offload_ctx {
	bool initialized;
	struct db_context *db_ctx;
};

NTSTATUS vfs_offload_token_ctx_init(TALLOC_CTX *mem_ctx,
				    struct vfs_offload_ctx **_ctx)
{
	struct vfs_offload_ctx *ctx = *_ctx;

	if (ctx != NULL) {
		if (!ctx->initialized) {
			return NT_STATUS_INTERNAL_ERROR;
		}
		return NT_STATUS_OK;
	}

	ctx = talloc_zero(mem_ctx, struct vfs_offload_ctx);
	if (ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	ctx->db_ctx = db_open_rbt(mem_ctx);
	if (ctx->db_ctx == NULL) {
		TALLOC_FREE(ctx);
		return NT_STATUS_INTERNAL_ERROR;
	}

	ctx->initialized = true;
	*_ctx = ctx;
	return NT_STATUS_OK;
}

struct fsp_token_link {
	struct vfs_offload_ctx *ctx;
	DATA_BLOB token_blob;
};

static int fsp_token_link_destructor(struct fsp_token_link *link)
{
	DATA_BLOB token_blob = link->token_blob;
	TDB_DATA key = make_tdb_data(token_blob.data, token_blob.length);
	NTSTATUS status;

	status = dbwrap_delete(link->ctx->db_ctx, key);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_ERR("dbwrap_delete failed: %s. Token:\n", nt_errstr(status));
		dump_data(0, token_blob.data, token_blob.length);
		return -1;
	}

	return 0;
}

NTSTATUS vfs_offload_token_db_store_fsp(struct vfs_offload_ctx *ctx,
					const files_struct *fsp,
					const DATA_BLOB *token_blob)
{
	struct db_record *rec = NULL;
	struct fsp_token_link *link = NULL;
	TDB_DATA key = make_tdb_data(token_blob->data, token_blob->length);
	TDB_DATA value;
	NTSTATUS status;

	rec = dbwrap_fetch_locked(ctx->db_ctx, talloc_tos(), key);
	if (rec == NULL) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	value = dbwrap_record_get_value(rec);
	if (value.dsize != 0) {
		void *ptr = NULL;
		files_struct *token_db_fsp = NULL;

		if (value.dsize != sizeof(ptr)) {
			DBG_ERR("Bad db entry for token:\n");
			dump_data(1, token_blob->data, token_blob->length);
			TALLOC_FREE(rec);
			return NT_STATUS_INTERNAL_ERROR;
		}
		memcpy(&ptr, value.dptr, value.dsize);
		TALLOC_FREE(rec);

		token_db_fsp = talloc_get_type_abort(ptr, struct files_struct);
		if (token_db_fsp != fsp) {
			DBG_ERR("token for fsp [%s] matches already known "
				"but different fsp [%s]:\n",
				fsp_str_dbg(fsp), fsp_str_dbg(token_db_fsp));
			dump_data(1, token_blob->data, token_blob->length);
			return NT_STATUS_INTERNAL_ERROR;
		}

		return NT_STATUS_OK;
	}

	link = talloc_zero(fsp, struct fsp_token_link);
	if (link == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	link->ctx = ctx;
	link->token_blob = data_blob_talloc(link, token_blob->data,
					    token_blob->length);
	if (link->token_blob.data == NULL) {
		TALLOC_FREE(link);
		return NT_STATUS_NO_MEMORY;
	}
	talloc_set_destructor(link, fsp_token_link_destructor);

	value = make_tdb_data((uint8_t *)&fsp, sizeof(files_struct *));

	status = dbwrap_record_store(rec, value, 0);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_ERR("dbwrap_record_store for [%s] failed: %s. Token\n",
			fsp_str_dbg(fsp), nt_errstr(status));
		dump_data(0, token_blob->data, token_blob->length);
		TALLOC_FREE(link);
		TALLOC_FREE(rec);
		return status;
	}

	TALLOC_FREE(rec);
	return NT_STATUS_OK;
}

NTSTATUS vfs_offload_token_db_fetch_fsp(struct vfs_offload_ctx *ctx,
					const DATA_BLOB *token_blob,
					files_struct **fsp)
{
	struct db_record *rec = NULL;
	TDB_DATA key = make_tdb_data(token_blob->data, token_blob->length);
	TDB_DATA value;
	void *ptr = NULL;

	rec = dbwrap_fetch_locked(ctx->db_ctx, talloc_tos(), key);
	if (rec == NULL) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	value = dbwrap_record_get_value(rec);
	if (value.dsize == 0) {
		DBG_DEBUG("Unknown token:\n");
		dump_data(10, token_blob->data, token_blob->length);
		TALLOC_FREE(rec);
		return NT_STATUS_OBJECT_NAME_NOT_FOUND;
	}

	if (value.dsize != sizeof(ptr)) {
		DBG_ERR("Bad db entry for token:\n");
		dump_data(1, token_blob->data, token_blob->length);
		TALLOC_FREE(rec);
		return NT_STATUS_INTERNAL_ERROR;
	}

	memcpy(&ptr, value.dptr, value.dsize);
	TALLOC_FREE(rec);

	*fsp = talloc_get_type_abort(ptr, struct files_struct);
	return NT_STATUS_OK;
}

NTSTATUS vfs_offload_token_create_blob(TALLOC_CTX *mem_ctx,
				       const files_struct *fsp,
				       uint32_t fsctl,
				       DATA_BLOB *token_blob)
{
	size_t len;

	switch (fsctl) {
	case FSCTL_DUP_EXTENTS_TO_FILE:
		len = 20;
		break;
	case FSCTL_SRV_REQUEST_RESUME_KEY:
		len = 24;
		break;
	default:
		DBG_ERR("Invalid fsctl [%" PRIu32 "]\n", fsctl);
		return NT_STATUS_NOT_SUPPORTED;
	}

	*token_blob = data_blob_talloc_zero(mem_ctx, len);
	if (token_blob->length == 0) {
		return NT_STATUS_NO_MEMORY;
	}

	/* combine persistent and volatile handles for the resume key */
	SBVAL(token_blob->data,  0, fsp->op->global->open_persistent_id);
	SBVAL(token_blob->data,  8, fsp->op->global->open_volatile_id);
	SIVAL(token_blob->data, 16, fsctl);

	return NT_STATUS_OK;
}
