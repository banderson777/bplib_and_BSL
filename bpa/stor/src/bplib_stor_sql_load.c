/* Local integration modifications (banderson777, 2026); see LOCAL_CHANGES.md. */
/*
 * NASA Docket No. GSC-19,559-1, and identified as "Delay/Disruption Tolerant Networking 
 * (DTN) Bundle Protocol (BP) v7 Core Flight System (cFS) Application Build 7.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this 
 * file except in compliance with the License. You may obtain a copy of the License at 
 *
 * http://www.apache.org/licenses/LICENSE-2.0 
 *
 * Unless required by applicable law or agreed to in writing, software distributed under 
 * the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF 
 * ANY KIND, either express or implied. See the License for the specific language 
 * governing permissions and limitations under the License. The copyright notice to be 
 * included in the software is as follows: 
 *
 * Copyright 2025 United States Government as represented by the Administrator of the 
 * National Aeronautics and Space Administration. All Rights Reserved.
 *
 */

#include "bplib_stor_sql.h"
#include "bplib_stor_sql_load.h"
#include "bplib_stor.h"
#include "bplib_inst.h"
#include "bplib_cbor.h"
#include "bplib_nc.h"
#include <stdlib.h>

#include <stdio.h>

/* ======= */
/* Globals */
/* ======= */

/* SQL query statements */


/* SQL query strings */

const char* FindBlobSQL =
"SELECT bundle_blobs.id, bundle_bytes, is_custodial, retransmit_trigger\n"
"FROM bundle_blobs JOIN bundle_data ON bundle_data.id = bundle_blobs.bundle_row\n"
"WHERE bundle_row = ? ORDER BY bundle_blobs.id;";


const char* MarkEgressedSQL =
"UPDATE bundle_data SET egress_attempted = 1 WHERE (id = ? AND is_custodial = 0);";

const char* MarkEgressedLocalSQL =
"UPDATE bundle_data SET egress_attempted = 1 WHERE (id = ?);";

const char* ResetRetransmitTriggerSQL =
"UPDATE bundle_data SET retransmit_timestamp = retransmit_trigger + ? WHERE (id = ? AND is_custodial = 1);";

const char *FindForEgressIdBaseSQL =
"SELECT id FROM bundle_data INDEXED BY idx_egress_id WHERE (%s) AND "
            "(egress_attempted = 0 AND action_timestamp > ?) AND ((is_custodial = 0) OR "
            "(is_custodial = 1 AND retransmit_trigger != ? AND retransmit_timestamp <= ?)) "
            "ORDER BY action_timestamp ASC LIMIT ?;";

const char *FindForEgressIdLocalBaseSQL = 
"SELECT id FROM bundle_data INDEXED BY idx_egress_id WHERE (%s) AND "
        "(egress_attempted = 0 AND action_timestamp > ?) "
        "ORDER BY action_timestamp ASC LIMIT ?;";

/* ==================== */
/* Function Definitions */
/* ==================== */

BPLib_Status_t BPLib_SQL_FindForEIDs(BPLib_Instance_t* Inst, BPLib_STOR_LoadBatch_t* Batch,
                        BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs, bool LocalDelivery)
{
    BPLib_Status_t Status;
    SQL_Status_t   SQLStatus;
    sqlite3*       db;
    char           WhereClause[BPLIB_SQL_MAX_STRLEN] = {0};

    Status      = BPLIB_SUCCESS;

    if ((Inst == NULL) || (Batch == NULL) || (DestEIDs == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Appended IDs become visible only if the complete selection succeeds.
     * Keep the configurable batch array off the stack; restoring its logical
     * bounds is sufficient because AddID never overwrites existing entries. */
    size_t InitialSize = Batch->Size;
    size_t InitialReadIndex = Batch->ReadIndex;
    db = Inst->BundleStorage.db;

    if ((NumEIDs == 0) || (NumEIDs > BPLIB_MAX_CONTACT_DEST_EIDS))
    {
        return BPLIB_STOR_PARAM_ERR;
    }
    
    /* Destination EIDs are set to dtn:none, don't bother searching database */
    if (DestEIDs[0].MaxNode == 0 && DestEIDs[0].MaxService == 0)
    {
        return BPLIB_SUCCESS;
    }

    Status = BPLib_SQL_GetDestEidWhereClause(DestEIDs, NumEIDs, WhereClause);
    if (Status != BPLIB_SUCCESS)
    {
        return Status;
    }

    /* 
    ** Build the final query. Note that "INDEXED BY idx_egress_id" is necessary to force
    ** sqlite to use the right index, otherwise it will try to use the much slower
    ** egress_attempted index by default, which will result in worse performance.
    */
    char *FindForEgressIdSQL = sqlite3_mprintf(
        LocalDelivery ? FindForEgressIdLocalBaseSQL : FindForEgressIdBaseSQL, WhereClause);
    if (FindForEgressIdSQL == NULL) return BPLIB_STOR_SQL_LOAD_IDS_ERR;
    sqlite3_stmt *FindForEgressIDStmt = NULL;

    /* Prepare Search Statements needed for this batch query */
    SQLStatus = sqlite3_prepare_v2(db, FindForEgressIdSQL, -1, &FindForEgressIDStmt, 0);
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Programming Error: FindForEgressIdSQL prepare failed, error=%s\n", sqlite3_errmsg(db));
        Status = BPLIB_STOR_SQL_LOAD_IDS_ERR;
    }

    if (Status == BPLIB_SUCCESS)
    {
        /* Run Batch Load Logic */
        SQLStatus = BPLib_SQL_FindForEIDsImpl(Inst, Batch, DestEIDs, NumEIDs, BPLIB_STOR_LOADBATCHSIZE, LocalDelivery, FindForEgressIDStmt);

        if (SQLStatus != SQLITE_OK)
        {
            Status = BPLIB_STOR_SQL_LOAD_IDS_ERR;
        }
    }

    /* Cleanup/Finalize */
    int FinalStatus = sqlite3_finalize(FindForEgressIDStmt);
    sqlite3_free(FindForEgressIdSQL);
    if (Status == BPLIB_SUCCESS && FinalStatus != SQLITE_OK)
        Status = BPLIB_STOR_SQL_LOAD_IDS_ERR;
    if (Status != BPLIB_SUCCESS)
    {
        Batch->Size = InitialSize;
        Batch->ReadIndex = InitialReadIndex;
    }

    return Status;
}

/** Step the prepared selection for the supplied endpoint ranges. Populate the load batch only
 * when the query completes successfully. */
SQL_Status_t BPLib_SQL_FindForEIDsImpl(BPLib_Instance_t* Inst, BPLib_STOR_LoadBatch_t* Batch,
                                        BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs,
                                        size_t MaxBundles, bool LocalDelivery, sqlite3_stmt *FindForEgressIDStmt)
{
    SQL_Status_t  SQLStatus;
    sqlite3*      db;
    uint64_t      CurrBundleRowID;
    size_t        i;
    uint64_t      BindIndex;

    db = Inst->BundleStorage.db;

    /* Bind parameters for metadata query */
    sqlite3_reset(FindForEgressIDStmt);

    BindIndex = 1;
    /* Bind destination EID query */
    for (i = 0; i < NumEIDs; i++)
    {
        if (DestEIDs[i].MaxNode == 0 && DestEIDs[i].MaxService == 0)
        {
            break;
        }

        SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, DestEIDs[i].MinNode);
        if (SQLStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to bind dest_node min: %s\n", sqlite3_errmsg(db));
            return SQLStatus;
        }

        if (DestEIDs[i].MinNode != DestEIDs[i].MaxNode)
        {
            SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, DestEIDs[i].MaxNode);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "Failed to bind dest_node max: %s\n", sqlite3_errmsg(db));
                return SQLStatus;
            }
        }

        SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, DestEIDs[i].MinService);
        if (SQLStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to bind dest_node min: %s\n", sqlite3_errmsg(db));
            return SQLStatus;
        }

        if (DestEIDs[i].MinService != DestEIDs[i].MaxService)
        {
            SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, DestEIDs[i].MaxService);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "Failed to bind dest_node max: %s\n", sqlite3_errmsg(db));
                return SQLStatus;
            }
        }
    }

    SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, BPLib_TIME_GetMonotonicTime());
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind action_timestamp: %s\n", sqlite3_errmsg(db));
        return SQLStatus;
    }

    /* Check for retransmission on foreign bundles */
    if (LocalDelivery == false)
    {
        /* Bind retransmit_trigger to garbage value */
        SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, BPLIB_NO_RETRANSMIT_TRIGGER);
        if (SQLStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to bind retransmit_trigger: %s\n", sqlite3_errmsg(db));
            return SQLStatus;
        }

        /* Bind retransmit_time to current time */
        SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, BPLib_TIME_GetMonotonicTime());
        if (SQLStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to bind retransmit_time: %s\n", sqlite3_errmsg(db));
            return SQLStatus;
        }
    }

    /* Bind maximum bundle limit */
    SQLStatus = sqlite3_bind_int64(FindForEgressIDStmt, BindIndex++, MaxBundles);
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind limit: %s\n", sqlite3_errmsg(db));
        return SQLStatus;
    }

    /* For every bundle found, place its ID in the load batch for the EgressID */
    SQLStatus = sqlite3_step(FindForEgressIDStmt);
    while (SQLStatus == SQLITE_ROW)
    {
        /* Load a single bundle from storage that matches the query */
        CurrBundleRowID = sqlite3_column_int64(FindForEgressIDStmt, 0);
        if (BPLib_STOR_LoadBatch_AddID(Batch, CurrBundleRowID) != BPLIB_SUCCESS)
        {
            break;
        }

        /* Go to the next row, which corresponds to the next bundle ID */
        SQLStatus = sqlite3_step(FindForEgressIDStmt);
    }

    if (SQLStatus == SQLITE_DONE)
    {
        /* For consistency with other helpers, convert DONE to OK */
        SQLStatus = SQLITE_OK;
    }

    /* Expecting SQLITE_OK */
    return SQLStatus;
}

/** Prepare and apply the egress update for a loaded batch. Release the statement and publish
 * accounting after successful commit. */
BPLib_Status_t BPLib_SQL_MarkBatchEgressed(BPLib_Instance_t *Inst,
                                         BPLib_STOR_LoadBatch_t *Batch, bool LocalDelivery)
{
    if (Inst == NULL || Batch == NULL) return BPLIB_NULL_PTR_ERROR;
    if (Batch->Size > BPLIB_STOR_LOADBATCHSIZE) return BPLIB_STOR_PARAM_ERR;
    if (Batch->Size == 0) return BPLIB_SUCCESS;
    sqlite3 *db = Inst->BundleStorage.db;
    sqlite3_stmt *MarkEgressedStmt = NULL;
    sqlite3_stmt *ResetRetransmitStmt = NULL;
    SQL_Status_t Status = sqlite3_prepare_v2(db,
        LocalDelivery ? MarkEgressedLocalSQL : MarkEgressedSQL, -1, &MarkEgressedStmt, NULL);
    if (Status == SQLITE_OK && !LocalDelivery)
        Status = sqlite3_prepare_v2(db, ResetRetransmitTriggerSQL, -1, &ResetRetransmitStmt, NULL);
    if (Status == SQLITE_OK)
        Status = BPLib_SQL_MarkBatchEgressedImpl(Inst, Batch, LocalDelivery, MarkEgressedStmt, ResetRetransmitStmt);
    sqlite3_finalize(MarkEgressedStmt);
    sqlite3_finalize(ResetRetransmitStmt);
    return Status == SQLITE_OK ? BPLIB_SUCCESS : BPLIB_STOR_SQL_MARK_EGRESSED_ERR;
}

/** Mark selected rows using the caller-owned statement within a transaction; roll back instead
 * of reporting a partial batch. */
SQL_Status_t BPLib_SQL_MarkBatchEgressedImpl(BPLib_Instance_t *Inst,
                                          BPLib_STOR_LoadBatch_t *Batch, bool LocalDelivery,
                                          sqlite3_stmt *MarkEgressedStmt, sqlite3_stmt *ResetRetransmitStmt)
{
    sqlite3 *db = Inst->BundleStorage.db;
    size_t NumEgressed = 0;
    if (Batch->Size == 0) return SQLITE_OK;
    SQL_Status_t Status = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    if (Status != SQLITE_OK) return Status;
    for (size_t Row = 0; Row < Batch->Size; ++Row)
    {
        sqlite3_reset(MarkEgressedStmt);
        Status = sqlite3_bind_int64(MarkEgressedStmt, 1, Batch->BundleRowIDs[Row]);
        if (Status != SQLITE_OK) goto rollback;
        Status = sqlite3_step(MarkEgressedStmt);
        if (Status != SQLITE_DONE) goto rollback;
        NumEgressed += sqlite3_changes(db);
    }
    if (!LocalDelivery)
    {
        for (size_t Row = 0; Row < Batch->Size; ++Row)
        {
            sqlite3_reset(ResetRetransmitStmt);
            Status = sqlite3_bind_int64(ResetRetransmitStmt, 1, BPLib_TIME_GetMonotonicTime());
            if (Status != SQLITE_OK) goto rollback;
            Status = sqlite3_bind_int64(ResetRetransmitStmt, 2, Batch->BundleRowIDs[Row]);
            if (Status != SQLITE_OK) goto rollback;
            Status = sqlite3_step(ResetRetransmitStmt);
            if (Status != SQLITE_DONE) goto rollback;
        }
    }
    Status = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if (Status != SQLITE_OK) goto rollback;
    Inst->BundleStorage.BundleCountNotEgressed -= NumEgressed;
    return SQLITE_OK;
rollback:
    sqlite3_reset(MarkEgressedStmt);
    if (!LocalDelivery) sqlite3_reset(ResetRetransmitStmt);
    if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) != SQLITE_OK)
        fprintf(stderr, "Failed to roll back egress batch: %s\n", sqlite3_errmsg(db));
    return Status;
}

/* New records contain wire bytes plus SQL scalar metadata, never C layouts. */
static BPLib_Status_t BPLib_SQL_LoadPortable(BPLib_Instance_t *Inst, int64_t Row,
                                           sqlite3_stmt *Info, BPLib_Bundle_t **Out)
{
    sqlite3_int64 Values[9];
    for (int i = 0; i < 9; ++i)
    {
        if (sqlite3_column_type(Info, i) != SQLITE_INTEGER) return BPLIB_STOR_SQL_LOAD_ERR;
        Values[i] = sqlite3_column_int64(Info, i);
    }
    /* version, ingress, local origin, wire length, custody flag, timer */
    if (Values[0] != 2 || Values[1] < 0 || Values[1] > UINT32_MAX ||
        (Values[2] != 0 && Values[2] != 1) || Values[3] <= 0 ||
        (uint64_t)Values[3] > BPLib_NC_GetNodeConfigValue(PARAM_SET_MAX_BUNDLE_LENGTH) ||
        (uint64_t)Values[3] > SIZE_MAX - sizeof(BPLib_BBlocks_t) ||
        (Values[4] != 0 && Values[4] != 1) || Values[5] < 0 || Values[5] > UINT32_MAX || Values[6] < 0 || Values[6] > UINT32_MAX ||
        Values[7] < 0 || Values[8] < 0 || Values[8] > UINT32_MAX)
        return BPLIB_STOR_SQL_LOAD_ERR;
    size_t Length = (size_t)Values[3], Offset = 0;
    unsigned char *Wire = malloc(Length);
    if (Wire == NULL) return BPLIB_STOR_NO_MEM_ERR;
    sqlite3_stmt *Chunks = NULL;
    int Rc = sqlite3_prepare_v2(Inst->BundleStorage.db,
        "SELECT blob_data FROM bundle_blobs WHERE bundle_row=? ORDER BY id;", -1, &Chunks, NULL);
    if (Rc == SQLITE_OK) Rc = sqlite3_bind_int64(Chunks, 1, Row);
    if (Rc == SQLITE_OK) Rc = sqlite3_step(Chunks);
    while (Rc == SQLITE_ROW)
    {
        int Bytes = sqlite3_column_bytes(Chunks, 0);
        const void *Data = sqlite3_column_blob(Chunks, 0);
        if (sqlite3_column_type(Chunks, 0) != SQLITE_BLOB || Bytes <= 0 || Data == NULL ||
            (size_t)Bytes > Length - Offset)
        { Rc = SQLITE_CORRUPT; break; }
        memcpy(Wire + Offset, Data, Bytes);
        Offset += Bytes;
        Rc = sqlite3_step(Chunks);
    }
    sqlite3_finalize(Chunks);
    BPLib_Status_t Status = BPLIB_STOR_SQL_LOAD_ERR;
    if (Rc == SQLITE_DONE && Offset == Length)
    {
        BPLib_Bundle_t *Bundle = BPLib_MEM_BundleAlloc(&Inst->pool, Wire, Length);
        if (Bundle == NULL) Status = BPLIB_STOR_NO_MEM_ERR;
        else
        {
            Status = BPLib_CBOR_DecodeBundle(Inst, Wire, Length, Bundle);
            if (Status == BPLIB_SUCCESS)
            {
                Bundle->blocks.PrimaryBlock.BundleId = (uint32_t)Values[6];
                Bundle->blocks.PrimaryBlock.MonoTime.Time = Values[7];
                Bundle->blocks.PrimaryBlock.MonoTime.BootEra = (uint32_t)Values[8];
                Bundle->Meta.IngressID = (uint32_t)Values[1];
                Bundle->Meta.EgressID = UINT32_MAX;
                Bundle->Meta.LocalBundle = Values[2] != 0;
                Bundle->Meta.IsCustodial = Values[4] != 0;
                Bundle->Meta.RetransmitTime = (uint32_t)Values[5];
                *Out = Bundle;
            }
            else
            {
                BPLib_MEM_BundleFree(&Inst->pool, Bundle);
                Status = Status == BPLIB_CBOR_DEC_ADMIN_RECORD_NULL_ERR ?
                         BPLIB_STOR_NO_MEM_ERR : BPLIB_STOR_SQL_LOAD_ERR;
            }
        }
    }
    else if (Rc == SQLITE_NOMEM) Status = BPLIB_STOR_NO_MEM_ERR;
    free(Wire);
    return Status;
}

/** Load one stored row using portable recovery metadata when present or the legacy reader
 * otherwise. A successful call transfers the allocated bundle to the caller. */
BPLib_Status_t BPLib_SQL_LoadBundle(BPLib_Instance_t* Inst, int64_t BundleRowID,
                                    BPLib_Bundle_t** Bundle)
{
    BPLib_Status_t Status;
    SQL_Status_t   SQLStatus;
    sqlite3*       db;

    if (Bundle == NULL) return BPLIB_NULL_PTR_ERROR;
    *Bundle = NULL;
    if (Inst == NULL) return BPLIB_NULL_PTR_ERROR;
    if (BundleRowID < 0) return BPLIB_STOR_PARAM_ERR;
    Status = BPLIB_SUCCESS;
    db = Inst->BundleStorage.db;

    sqlite3_stmt *Info = NULL;
    SQLStatus = sqlite3_prepare_v2(db,
        "SELECT r.format,r.ingress_id,r.local_bundle,r.wire_bytes,b.is_custodial,b.retransmit_trigger,b.bundle_id,r.receipt_time,r.receipt_era "
        "FROM bundle_recovery r JOIN bundle_data b ON b.id=r.bundle_row WHERE b.id=?;",
        -1, &Info, NULL);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_bind_int64(Info, 1, BundleRowID);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_step(Info);
    if (SQLStatus == SQLITE_ROW)
    {
        Status = BPLib_SQL_LoadPortable(Inst, BundleRowID, Info, Bundle);
        sqlite3_finalize(Info);
        return Status;
    }
    sqlite3_finalize(Info);
    if (SQLStatus != SQLITE_DONE)
        return SQLStatus == SQLITE_NOMEM ? BPLIB_STOR_NO_MEM_ERR : BPLIB_STOR_SQL_LOAD_ERR;

    sqlite3_stmt *FindBlobStmt = NULL;
    SQLStatus = sqlite3_prepare_v2(db, FindBlobSQL, -1, &FindBlobStmt, 0);
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Programming Error: FindBlobSQL prepare failed, error=%s\n", sqlite3_errmsg(db));
        Status = BPLIB_STOR_SQL_LOAD_ERR;
    }

    if (Status == BPLIB_SUCCESS)
    {
        SQLStatus = BPLib_SQL_LoadBundleImpl(Inst, BundleRowID, Bundle, FindBlobStmt);
    }

    sqlite3_finalize(FindBlobStmt);

    if (SQLStatus == SQLITE_NOMEM)
    {
        Status = BPLIB_STOR_NO_MEM_ERR;
    }
    else if (SQLStatus != SQLITE_OK && SQLStatus != SQLITE_DONE)
    {
        Status = BPLIB_STOR_SQL_LOAD_ERR;
    }
    else if (*Bundle == NULL)
    {
        Status = BPLIB_STOR_NO_BUNDLE_FOUND_ERR;
    }

    return Status;
}

/** Read legacy metadata and blob blocks into a reconstructed bundle. Release the partially
 * built chain if any row or allocation fails. */
SQL_Status_t BPLib_SQL_LoadBundleImpl(BPLib_Instance_t* Inst, int64_t BundleRowID,
                                        BPLib_Bundle_t** Bundle, sqlite3_stmt *FindBlobStmt)
{
    SQL_Status_t       SQLStatus;
    sqlite3_blob*      blob;
    int64_t            BlobRowId;
    size_t             ChunkSize;
    size_t             LoadedBytes = sizeof(BPLib_BBlocks_t);
    BPLib_MEM_Block_t* BundleHead;
    BPLib_MEM_Block_t* CurrBlock;
    BPLib_MEM_Block_t* NextBlock;
    BPLib_Bundle_t*    RetBundle;
    sqlite3*           db;
    BPLib_MEM_Pool_t*  Pool;

    *Bundle    = NULL;
    blob       = NULL;
    BundleHead = NULL;
    CurrBlock  = NULL;
    NextBlock  = NULL;
    db         = Inst->BundleStorage.db;
    Pool       = &Inst->pool;

    sqlite3_reset(FindBlobStmt);

    SQLStatus = sqlite3_bind_int64(FindBlobStmt, 1, BundleRowID);
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "bind failed: %s\n", sqlite3_errmsg(db));
        return SQLStatus;
    }

    /* Load each block of the blob into a mem pool block */
    while ((SQLStatus = sqlite3_step(FindBlobStmt)) == SQLITE_ROW)
    {
        /* Determine the row ID of the blob in the bundle_blobs table */
        BlobRowId = sqlite3_column_int64(FindBlobStmt, 0);

        /* We're loading bblocks_t */
        if (BundleHead == NULL)
        {
            /* Open the metadata using the blob streaming API */
            SQLStatus = sqlite3_blob_open(db, "main", "bundle_blobs", "blob_data", BlobRowId, 0, &blob);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "sqlite3_blob_open failed for rowid %ld: %s\n", BlobRowId, sqlite3_errmsg(db));
                break;
            }

            /* Ensure the metadata size exactly matches bblocks_t size. Otherwise, don't bother loading it */
            ChunkSize = sqlite3_blob_bytes(blob);
            if (ChunkSize != sizeof(BPLib_BBlocks_t))
            {
                fprintf(stderr, "Expected to read metadata chunk and got wrong size %lu != %lu\n",
                        ChunkSize, sizeof(BPLib_BBlocks_t));

                SQLStatus = SQLITE_CORRUPT;

                sqlite3_blob_close(blob);
                break;
            }

            /* Allocate a MEM pool block for the meta data */
            BundleHead = BPLib_MEM_BlockAlloc(Pool, BPLIB_MEM_BIG_BLK_SIZE);
            if (BundleHead == NULL)
            {
                SQLStatus = SQLITE_NOMEM;
                sqlite3_blob_close(blob);
                break;
            }

            /* Pool user data may be recycled. Restore persisted fields and give
             * unavailable contact/origin data deterministic, non-routing defaults. */
            memset(&BundleHead->user_data.Bundle, 0, sizeof(BPLib_Bundle_t));
            RetBundle = &BundleHead->user_data.Bundle;
            sqlite3_int64 StoredBytes = sqlite3_column_int64(FindBlobStmt, 1);
            sqlite3_int64 Custodial = sqlite3_column_int64(FindBlobStmt, 2);
            sqlite3_int64 Trigger = sqlite3_column_int64(FindBlobStmt, 3);
            if (sqlite3_column_type(FindBlobStmt, 1) != SQLITE_INTEGER ||
                sqlite3_column_type(FindBlobStmt, 2) != SQLITE_INTEGER ||
                sqlite3_column_type(FindBlobStmt, 3) != SQLITE_INTEGER ||
                StoredBytes < 0 || (uint64_t)StoredBytes > SIZE_MAX ||
                (Custodial != 0 && Custodial != 1) || Trigger < 0 || Trigger > UINT32_MAX)
            {
                SQLStatus = SQLITE_CORRUPT;
                sqlite3_blob_close(blob);
                break;
            }
            RetBundle->Meta.TotalBytes = (size_t)StoredBytes;
            RetBundle->Meta.IsCustodial = Custodial != 0;
            RetBundle->Meta.RetransmitTime = (uint32_t)Trigger;
            RetBundle->Meta.IngressID = UINT32_MAX;
            RetBundle->Meta.EgressID = UINT32_MAX;

            /* Load the metadata directly into the mempool block */
            SQLStatus = sqlite3_blob_read(blob, (void*)&(BundleHead->user_data.Bundle.blocks), ChunkSize, 0);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "sqlite3_blob_read failed: %s\n", sqlite3_errmsg(db));
                sqlite3_blob_close(blob);
                break;
            }

            /* Serialized process addresses cannot own objects in this process.
             * Keep legacy addresses opaque until the wire chunks are loaded;
             * reconstruct owned administrative records before returning. */

            /* Load succeeded */
            CurrBlock = BundleHead;
            CurrBlock->used_len = ChunkSize;
        }
        else
        { /* We're loading part of the blob */
            /* Open the metadata using the blob streaming API */
            SQLStatus = sqlite3_blob_open(db, "main", "bundle_blobs", "blob_data", BlobRowId, 0, &blob);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "sqlite3_blob_open failed for rowid %ld: %s\n", BlobRowId, sqlite3_errmsg(db));
                break;
            }

            /* Allocate a MEM pool block for the blob data */
            NextBlock = BPLib_MEM_BlockAlloc(Pool, BPLIB_MEM_BIG_BLK_SIZE);
            if (NextBlock == NULL)
            {
                SQLStatus = SQLITE_NOMEM;
                sqlite3_blob_close(blob);
                break;
            }

            CurrBlock->next = NextBlock;

            /* Make sure the chunk isn't larger than the buffer. This could happen if a previous
            ** database or BPLib version had a different chunk size. We can't support this case safely. User
            ** will have to create a new database.
            */
            ChunkSize = sqlite3_blob_bytes(blob);
            if (ChunkSize == 0 || ChunkSize > BPLIB_MEM_BIG_BLK_DATA_SIZE)
            {
                fprintf(stderr, "Stored BLOB is too large for buffer. DB is corrupted %lu > %lu\n",
                        ChunkSize, BPLIB_MEM_BIG_BLK_DATA_SIZE);

                SQLStatus = SQLITE_CORRUPT;

                sqlite3_blob_close(blob);
                break;
            }


            /* Load the blob directly into the mempool block */
            SQLStatus = sqlite3_blob_read(blob, (void*)&NextBlock->user_data.BigData, ChunkSize, 0);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "sqlite3_blob_read failed: %s\n", sqlite3_errmsg(db));
                sqlite3_blob_close(blob);
                break;
            }

            if (LoadedBytes > SIZE_MAX - ChunkSize)
            {
                SQLStatus = SQLITE_CORRUPT;
                sqlite3_blob_close(blob);
                break;
            }
            LoadedBytes += ChunkSize;
            /* Load Succeeded */
            CurrBlock = NextBlock;
            CurrBlock->used_len = ChunkSize;
        }

        sqlite3_blob_close(blob);
    }

    if (BundleHead == NULL)
    {
        *Bundle = NULL;

        /*
        ** Leave status code as is, could be either a lack of memory issue (SQLITE_NOMEM)
        ** or an indication that the bundle no longer exists in storage
        */
    }
    else if (SQLStatus == SQLITE_DONE && (BundleHead->next == NULL ||
             LoadedBytes != BundleHead->user_data.Bundle.Meta.TotalBytes))
    {
        /* Missing wire data or inconsistent persisted byte accounting. */
        SQLStatus = SQLITE_CORRUPT;
        BPLib_MEM_BlockListFree(Pool, BundleHead);
    }
    else if (SQLStatus == SQLITE_DONE)
    { /* Expecting SQLITE_DONE */
        RetBundle       = &(BundleHead->user_data.Bundle);
        RetBundle->blob = BundleHead->next;
        if (RetBundle->blocks.AdminRecordPayload != NULL)
        {
            /* Legacy pointer values are ignored; reconstruct from owned wire. */
            RetBundle->blocks.AdminRecordPayload = NULL;
            size_t WireLength = LoadedBytes - sizeof(BPLib_BBlocks_t);
            unsigned char *Wire = NULL;
            if (WireLength <= BPLib_NC_GetNodeConfigValue(PARAM_SET_MAX_BUNDLE_LENGTH))
                Wire = malloc(WireLength);
            if (Wire == NULL)
            {
                BPLib_MEM_BlockListFree(Pool, BundleHead);
                return WireLength > BPLib_NC_GetNodeConfigValue(PARAM_SET_MAX_BUNDLE_LENGTH) ? SQLITE_CORRUPT : SQLITE_NOMEM;
            }
            size_t Offset = 0;
            for (CurrBlock = RetBundle->blob; CurrBlock != NULL; CurrBlock = CurrBlock->next)
            {
                memcpy(Wire + Offset, CurrBlock->user_data.BigData, CurrBlock->used_len);
                Offset += CurrBlock->used_len;
            }
            uint32_t BundleId = RetBundle->blocks.PrimaryBlock.BundleId;
            BPLib_TIME_MonotonicTime_t ReceiptTime = RetBundle->blocks.PrimaryBlock.MonoTime;
            memset(&RetBundle->blocks, 0, sizeof(RetBundle->blocks));
            BPLib_Status_t DecodeStatus = BPLib_CBOR_DecodeBundle(Inst, Wire, WireLength, RetBundle);
            free(Wire);
            RetBundle->blocks.PrimaryBlock.BundleId = BundleId;
            RetBundle->blocks.PrimaryBlock.MonoTime = ReceiptTime;
            if (DecodeStatus != BPLIB_SUCCESS)
            {
                BPLib_MEM_BundleFree(Pool, RetBundle);
                return DecodeStatus == BPLIB_CBOR_DEC_ADMIN_RECORD_NULL_ERR ? SQLITE_NOMEM : SQLITE_CORRUPT;
            }
        }
        *Bundle         = RetBundle;

        /* For consistency with other helpers, set status to SQLITE_OK */
        SQLStatus = SQLITE_OK;
    }
    else
    {
        /* Something in the loop above went wrong: Free memory */
        BPLib_MEM_BlockListFree(Pool, BundleHead);
    }

    /* Expecting SQLITE_OK */
    return SQLStatus;
}
