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

/* ======== */
/* Includes */
/* ======== */

#include "bplib_stor_sql.h"
#include "bplib_as.h"
#include "bplib_nc.h"
#include "bplib_stor_sql_store.h"
#include "bplib_inst.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "bplib_cbor.h"
#include "bplib_eid.h"

/* ======= */
/* Globals */
/* ======= */

/* SQL query strings */

/* Insert Bundle Blob */
const char* InsertBlobSQL =
"INSERT INTO bundle_blobs (bundle_row, blob_data) VALUES (?, ?)";

/* Insert Bundle Metadata (duplicate bundle_id entries are ignored) */
const char* InsertMetadataSQL =
"INSERT INTO bundle_data (bundle_id, action_timestamp, retransmit_timestamp, \n"
"retransmit_trigger, dest_node, dest_service, is_custodial, bundle_bytes) \n"
"VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

/* ================ */
/* Helper Functions */
/* ================ */

#if 0
/* 
** Helper function to print contents of sqlite bundle_data table for debugging. Should be
** kept commented out in main. Use by calling:
**      sqlite3_exec(db, "SELECT * FROM bundle_data;", BPLib_SQL_PrintTbl, 0, 0);
*/
static int BPLib_SQL_PrintTbl(void* data, int argc, char** argv, char** azColName)
{
    int i;

    printf("*******************\n");
    fprintf(stderr, "%s: ", (const char*)data);

    for (i = 0; i < argc; i++) {
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }

    printf("*******************\n");
    return 0;
}
#endif 

/* ==================== */
/* Function Definitions */
/* ==================== */

SQL_Status_t BPLib_SQL_StoreMetadata(BPLib_Bundle_t* Bundle, sqlite3_stmt *InsertMetadataStmt)
{
    SQL_Status_t         SQLStatus;
    BPLib_PrimaryBlock_t *PrimaryBlock;
    uint64_t             AgeBlockTime;
    uint64_t             ExpirationTime;
    uint64_t             MonoTimeAge;
    uint64_t             MonoTimeRemaining;
    uint16_t             ExtensionBlockIdx;
    uint64_t             EffectiveLifetime;

    PrimaryBlock   = &(Bundle->blocks.PrimaryBlock);
    ExpirationTime = 0;

    /* Ensure the lifetime is less than or equal to the max allowed lifetime */
    EffectiveLifetime = BPLib_NC_GetNodeConfigValue(PARAM_SET_MAX_LIFETIME);

    if (EffectiveLifetime > PrimaryBlock->Lifetime)
    {
        EffectiveLifetime = PrimaryBlock->Lifetime;
    }

    sqlite3_reset(InsertMetadataStmt);

    /* Bind bundle_id to InsertMetadataStmt */
    SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 1, 
                                    (int64_t)PrimaryBlock->BundleId);

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind bundle_id in store_meta\n");
    }
    else
    {
        /* Add the value of the timestamp used as an indicator for some action to the InsertMetadataStmt variable (action_timestamp) */
        
        if (PrimaryBlock->Timestamp.CreateTime != 0)
        { /* Bundle has a valid creation time */
            if (BPLib_TIME_GetCurrentDtnTime() != 0)
            { /* DTN time is valid */
                MonoTimeAge       = BPLib_TIME_GetCurrentDtnTime() - PrimaryBlock->Timestamp.CreateTime;
                MonoTimeRemaining = EffectiveLifetime - MonoTimeAge;
                ExpirationTime    = BPLib_TIME_GetMonotonicTime()  + MonoTimeRemaining;
            }
            else
            { /* DTN time is invalid */
                ExpirationTime = PrimaryBlock->MonoTime.Time + EffectiveLifetime;
            }
        }
        else
        { /* Bundle creation time is invalid */
            for (ExtensionBlockIdx = 0; ExtensionBlockIdx < BPLIB_MAX_NUM_EXTENSION_BLOCKS; ExtensionBlockIdx++)
            {
                if (Bundle->blocks.ExtBlocks[ExtensionBlockIdx].Header.BlockType == BPLib_BlockType_Age)
                {
                    MonoTimeAge    = PrimaryBlock->MonoTime.Time + EffectiveLifetime;
                    AgeBlockTime   = Bundle->blocks.ExtBlocks[ExtensionBlockIdx].BlockData.AgeBlockData.Age;
                    ExpirationTime = MonoTimeAge - AgeBlockTime;

                    break;
                }
            }
        }
        
        SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 2, (sqlite3_int64) ExpirationTime);
    }

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind action_timestamp in store_meta\n");
    }
    else
    {
        /* Add the retransmission timestamp into the InsertMetadataStmt variable */
        SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 3, 
                                (int64_t)BPLib_TIME_GetMonotonicTime() + Bundle->Meta.RetransmitTime);
    }
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind retransmit_timestamp in store_meta\n");
    }
    else
    {
        /* Add the retransmission time into the InsertMetadataStmt variable */
        SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 4, Bundle->Meta.RetransmitTime);
    }
    
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind retransmit_trigger in store_meta\n");
    }
    else
    {
        /* Add the destination node into the InsertMetadataStmt variable */
        SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 5, 
                                (int64_t)PrimaryBlock->DestEID.Node);
    }

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind dest_node in store_meta\n");
    }
    else
    {
        /* Add the destination service into the InsertMetadataStmt variable */
        SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 6, 
                                (int64_t)PrimaryBlock->DestEID.Service);
    }

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind dest_service in store_meta\n");
    }
    else
    {
        /* Add the bundle type to the InsertMetadataStmt variable */
        SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 7, (int64_t)Bundle->Meta.IsCustodial);
    }

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind is_custodial in store_meta\n");
    }
    else
    {
        /* Add the bundle size in bytes into the InsertMetadataStmt variable */
        SQLStatus = sqlite3_bind_int64(InsertMetadataStmt, 8, (int64_t)Bundle->Meta.TotalBytes);
    }    

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind bundle_size in store_meta\n");
    }
    else
    {
        /* Evaluate provided sqlite statement */
        SQLStatus = sqlite3_step(InsertMetadataStmt);
    }

    /* Expecting SQLITE_DONE */
    return SQLStatus;
}

SQL_Status_t BPLib_SQL_StoreChunk(int64_t BundleRowID, const void* Chunk, size_t ChunkSize, sqlite3_stmt *InsertBlobStmt)
{
    if (InsertBlobStmt == NULL) return SQLITE_MISUSE;
    /* Reset may report the prior step error while still resetting the statement.
     * Clear all parameters before reuse, including parameters bound by a caller. */
    (void) sqlite3_reset(InsertBlobStmt);
    SQL_Status_t SQLStatus = sqlite3_clear_bindings(InsertBlobStmt);
    if (SQLStatus != SQLITE_OK) return SQLStatus;
    if (Chunk == NULL) return SQLITE_MISUSE;
    if (ChunkSize > INT_MAX) return SQLITE_TOOBIG;

    SQLStatus = sqlite3_bind_int64(InsertBlobStmt, 1, BundleRowID);
    if (SQLStatus == SQLITE_OK)
        SQLStatus = sqlite3_bind_blob(InsertBlobStmt, 2, Chunk, (int)ChunkSize, SQLITE_STATIC);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_step(InsertBlobStmt);
    if (SQLStatus == SQLITE_DONE && sqlite3_changes(sqlite3_db_handle(InsertBlobStmt)) != 1)
        SQLStatus = SQLITE_CORRUPT;

    /* SQLITE_STATIC borrows the caller's memory. Release it before StoreBundle
     * frees the encoded wire buffer, on both success and failure. */
    SQL_Status_t ResetStatus = sqlite3_reset(InsertBlobStmt);
    SQL_Status_t ClearStatus = sqlite3_clear_bindings(InsertBlobStmt);
    if (SQLStatus == SQLITE_DONE && ResetStatus != SQLITE_OK) SQLStatus = ResetStatus;
    if (SQLStatus == SQLITE_DONE && ClearStatus != SQLITE_OK) SQLStatus = ClearStatus;
    return SQLStatus;
}

SQL_Status_t BPLib_SQL_StoreBundle(sqlite3* db, BPLib_Bundle_t* Bundle, BPLib_SQL_StoreStatements_t *Statements)
{
    /* Locally created bundles may own just an ADU blob and dirty decoded
     * headers. Persist a complete encoding, not that implementation detail. */
    size_t Capacity = BPLib_NC_GetNodeConfigValue(PARAM_SET_MAX_BUNDLE_LENGTH);
    unsigned char *Wire = malloc(Capacity);
    if (Wire == NULL) return SQLITE_NOMEM;
    size_t WireBytes = 0, OriginalBytes = Bundle->Meta.TotalBytes;
    SQL_Status_t SQLStatus = SQLITE_CORRUPT;
    BPLib_Bundle_t Snapshot = *Bundle;
    if (Snapshot.Meta.LocalBundle)
    {
        /* Local block factories leave these EIDs unset until first egress.
         * Give the persisted encoding valid local placeholders; egress assigns
         * the actual custody sequence and updates forwarding fields as usual. */
        for (size_t i = 0; i < BPLIB_MAX_NUM_EXTENSION_BLOCKS; ++i)
        {
            BPLib_ExtensionBlock_t *Block = &Snapshot.blocks.ExtBlocks[i];
            if (Block->Header.BlockType == BPLib_BlockType_PrevNode &&
                !BPLib_EID_IsValid(&Block->BlockData.PrevNodeBlockData.PrevNodeId))
                Block->BlockData.PrevNodeBlockData.PrevNodeId = BPLIB_EID_INSTANCE;
            if (Block->Header.BlockType == BPLib_BlockType_CTEB &&
                !BPLib_EID_IsValid(&Block->BlockData.CustodyBlockData.BlockSrcAdminEID))
                Block->BlockData.CustodyBlockData.BlockSrcAdminEID = BPLIB_EID_INSTANCE;
        }
    }
    if (BPLib_CBOR_EncodeBundle(&Snapshot, Wire, Capacity, &WireBytes) != BPLIB_SUCCESS ||
        WireBytes == 0 || WireBytes > Capacity || WireBytes > INT64_MAX ||
        WireBytes > SIZE_MAX - sizeof(BPLib_BBlocks_t)) goto done;
    Bundle->Meta.TotalBytes = WireBytes + sizeof(BPLib_BBlocks_t);
    SQLStatus = BPLib_SQL_StoreMetadata(Bundle, Statements->Metadata);
    if (SQLStatus != SQLITE_DONE) goto done;
    if (sqlite3_changes(db) != 1) { SQLStatus = SQLITE_CORRUPT; goto done; }
    sqlite3_int64 BundleRowID = sqlite3_last_insert_rowid(db);
    sqlite3_stmt *RecoveryStmt = NULL;
    SQLStatus = sqlite3_prepare_v2(db,
        "INSERT INTO bundle_recovery(bundle_row,format,ingress_id,local_bundle,wire_bytes,receipt_time,receipt_era) VALUES(?,2,?,?,?,?,?);",
        -1, &RecoveryStmt, NULL);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_bind_int64(RecoveryStmt, 1, BundleRowID);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_bind_int64(RecoveryStmt, 2, Bundle->Meta.IngressID);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_bind_int(RecoveryStmt, 3, Bundle->Meta.LocalBundle);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_bind_int64(RecoveryStmt, 4, WireBytes);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_bind_int64(RecoveryStmt, 5, Bundle->blocks.PrimaryBlock.MonoTime.Time);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_bind_int64(RecoveryStmt, 6, Bundle->blocks.PrimaryBlock.MonoTime.BootEra);
    if (SQLStatus == SQLITE_OK) SQLStatus = sqlite3_step(RecoveryStmt);
    if (SQLStatus == SQLITE_DONE && sqlite3_changes(db) != 1) SQLStatus = SQLITE_CORRUPT;
    sqlite3_finalize(RecoveryStmt);
    if (SQLStatus != SQLITE_DONE) goto done;
    for (size_t Offset = 0; Offset < WireBytes;)
    {
        size_t Bytes = WireBytes - Offset;
        if (Bytes > BPLIB_MEM_BIG_BLK_DATA_SIZE) Bytes = BPLIB_MEM_BIG_BLK_DATA_SIZE;
        SQLStatus = BPLib_SQL_StoreChunk(BundleRowID, Wire + Offset, Bytes, Statements->Blob);
        if (SQLStatus != SQLITE_DONE) break;
        Offset += Bytes;
    }
done:
    if (SQLStatus != SQLITE_DONE) Bundle->Meta.TotalBytes = OriginalBytes;
    free(Wire);
    return SQLStatus;
}

/* Detect an already-present ID before attempting any writes. Later constraints
 * are write failures, never evidence that the new bundle is redundant. */
static SQL_Status_t BPLib_SQL_BundleExists(sqlite3_stmt *Statement, uint32_t BundleId, bool *Exists)
{
    *Exists = false;
    SQL_Status_t Status = sqlite3_reset(Statement);
    SQL_Status_t Clear = sqlite3_clear_bindings(Statement);
    if (Status != SQLITE_OK) return Status;
    if (Clear != SQLITE_OK) return Clear;
    Status = sqlite3_bind_int64(Statement, 1, BundleId);
    if (Status == SQLITE_OK) Status = sqlite3_step(Statement);
    bool Found = false;
    if (Status == SQLITE_ROW) { Found = sqlite3_column_int(Statement, 0) != 0; Status = SQLITE_OK; }
    else if (Status == SQLITE_DONE) Status = SQLITE_CORRUPT;
    SQL_Status_t Reset = sqlite3_reset(Statement);
    Clear = sqlite3_clear_bindings(Statement);
    if (Status == SQLITE_OK && Reset != SQLITE_OK) Status = Reset;
    if (Status == SQLITE_OK && Clear != SQLITE_OK) Status = Clear;
    if (Status == SQLITE_OK) *Exists = Found;
    return Status;
}

SQL_Status_t BPLib_SQL_StoreImpl(BPLib_Instance_t* Inst, size_t *TotalBytesStored,
                                size_t *DuplicateBundlesIgnored, size_t *CustodialBundles,
                                BPLib_SQL_StoreStatements_t *Statements)
{
    SQL_Status_t SQLStatus;
    size_t       i;
    sqlite3*     db;
    size_t       NewBundleBytes;

    db = Inst->BundleStorage.db;
    *TotalBytesStored = 0;
    *DuplicateBundlesIgnored = 0;
    *CustodialBundles = 0;

    if (Inst->BundleStorage.InsertBatchSize == 0)
    {
        return SQLITE_OK;
    }

    /* Create a batch query */
    SQLStatus = sqlite3_exec(db, "BEGIN;", 0, 0, 0);
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to start transaction: %s\n", sqlite3_errmsg(db));
        return SQLStatus;
    }

    /* Perform an insert for every bundle */
    for (i = 0; i < Inst->BundleStorage.InsertBatchSize; i++)
    {
        bool Exists;
        SQLStatus = BPLib_SQL_BundleExists(Statements->Lookup,
            Inst->BundleStorage.InsertBatch[i]->blocks.PrimaryBlock.BundleId, &Exists);
        if (SQLStatus != SQLITE_OK) break;
        if (Exists)
        {
            (*DuplicateBundlesIgnored)++;
            SQLStatus = SQLITE_DONE;
            continue;
        }
        NewBundleBytes = Inst->BundleStorage.InsertBatch[i]->Meta.TotalBytes;
        if (NewBundleBytes > BPLIB_MAX_STORED_BUNDLE_BYTES ||
            Inst->BundleStorage.BytesStorageInUse > BPLIB_MAX_STORED_BUNDLE_BYTES - NewBundleBytes ||
            *TotalBytesStored > BPLIB_MAX_STORED_BUNDLE_BYTES - NewBundleBytes - Inst->BundleStorage.BytesStorageInUse)
        {
            SQLStatus = SQLITE_FULL;
            break;
        }

        SQLStatus = BPLib_SQL_StoreBundle(db, Inst->BundleStorage.InsertBatch[i], Statements);
        if (SQLStatus == SQLITE_DONE)
        {
            NewBundleBytes = Inst->BundleStorage.InsertBatch[i]->Meta.TotalBytes;
            if (NewBundleBytes > BPLIB_MAX_STORED_BUNDLE_BYTES ||
                Inst->BundleStorage.BytesStorageInUse > BPLIB_MAX_STORED_BUNDLE_BYTES - NewBundleBytes ||
                *TotalBytesStored > BPLIB_MAX_STORED_BUNDLE_BYTES - NewBundleBytes - Inst->BundleStorage.BytesStorageInUse)
            { SQLStatus = SQLITE_FULL; break; }
            *TotalBytesStored += NewBundleBytes;

            if (Inst->BundleStorage.InsertBatch[i]->Meta.IsCustodial)
            {
                (*CustodialBundles)++;
            }
        }
        else
        {
            /* Any write failure rolls back the batch, including UNIQUE errors
             * unrelated to an existing bundle ID and trigger-suppressed writes. */
            break;
        }
    }

    /* If there have been no errors so far, batch-write the data to persistent storage */
    if (SQLStatus == SQLITE_DONE)
    {
        SQLStatus = sqlite3_exec(db, "COMMIT;", 0, 0, 0);
        if (SQLStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to commit transaction\n");
        }
    }

    /* Preserve the failed write/COMMIT result. A successful ROLLBACK does
     * not mean the batch was stored or permit custody acceptance. */
    if (SQLStatus != SQLITE_OK)
    {
        if (SQLStatus != SQLITE_FULL)
        {
            fprintf(stderr, "Batch commit failed, RC=%d\n", SQLStatus);
        }
        SQL_Status_t RollbackStatus = sqlite3_exec(db, "ROLLBACK;", 0, 0, 0);
        if (RollbackStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to rollback transaction, RC=%d\n", RollbackStatus);
        }
        *TotalBytesStored = 0;
        *DuplicateBundlesIgnored = 0;
        *CustodialBundles = 0;
    }

    /* Expecting SQLITE_OK */
    return SQLStatus;
}

BPLib_Status_t BPLib_SQL_Store(BPLib_Instance_t* Inst, size_t *TotalBytesStored,
                                size_t *DuplicateBundlesIgnored, size_t *CustodialBundles)
{
    BPLib_Status_t Status;
    SQL_Status_t   SQLStatus;
    sqlite3*       db;
    BPLib_SQL_StoreStatements_t Statements = {0};

    Status = BPLIB_SUCCESS;
    db     = Inst->BundleStorage.db;
    *TotalBytesStored = 0;
    *DuplicateBundlesIgnored = 0;
    *CustodialBundles = 0;

    /* Prepare Insert Statements needed for this batch query */
    SQLStatus = sqlite3_prepare_v2(db, InsertMetadataSQL, -1, &Statements.Metadata, 0);
    if (SQLStatus != SQLITE_OK)
    {
        Status = BPLIB_STOR_SQL_STORAGE_ERR;
    }

    SQLStatus = sqlite3_prepare_v2(db, InsertBlobSQL, -1, &Statements.Blob, 0);
    if (SQLStatus != SQLITE_OK)
    {
        Status = BPLIB_STOR_SQL_STORAGE_ERR;
    }

    if (Status == BPLIB_SUCCESS)
    {
        SQLStatus = sqlite3_prepare_v2(db,
            "SELECT EXISTS(SELECT 1 FROM bundle_data WHERE bundle_id=?);", -1, &Statements.Lookup, NULL);
        if (SQLStatus != SQLITE_OK) Status = BPLIB_STOR_SQL_STORAGE_ERR;
    }

    if (Status == BPLIB_SUCCESS)
    {
        /* Run the batch storage logic */
        SQLStatus = BPLib_SQL_StoreImpl(Inst, TotalBytesStored, DuplicateBundlesIgnored, CustodialBundles, &Statements);
        if (SQLStatus == SQLITE_FULL)
        {
            Status = BPLIB_STOR_DB_FULL_ERR;
        }
        else if (SQLStatus != SQLITE_OK)
        {
            Status = BPLIB_STOR_SQL_STORAGE_ERR;
        }
    }

    /* Finalize */
    sqlite3_finalize(Statements.Metadata);
    sqlite3_finalize(Statements.Blob);
    sqlite3_finalize(Statements.Lookup);

    return Status;
}
