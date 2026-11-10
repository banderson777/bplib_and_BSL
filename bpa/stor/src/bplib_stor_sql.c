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
#include "bplib_time.h"
#include "bplib_fwp.h"
#include "bplib_inst.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/* Bound transient contention from read-only monitors and other connections.
 * Missions may override this at compile time; no lock is waited on forever. */
#ifndef BPLIB_SQL_BUSY_TIMEOUT_MSEC
#define BPLIB_SQL_BUSY_TIMEOUT_MSEC 1000
#endif

/* ======= */
/* Globals */
/* ======= */

/* SQL query statements */


/* SQL query strings */

/*
** Table and Index Creation for bundle_data and bundle_blobs
**
** This schema is designed to support efficient queries and operations on bundle metadata and associated blob data.
** The following indexes are created:
**
** 1. idx_bundle_blobs_bundle_row:
**    - Index on the 'bundle_row' column in the 'bundle_blobs' table. This index supports quick lookup of blob data
**      by its associated bundle_row in the 'bundle_data' table.
**
** 2. idx_action_timestamp:
**    - Index on 'action_timestamp' in the 'bundle_data' table. This helps with queries that need to sort or filter
**      based on the timestamp of the bundle: This is used for expiring bundles
**
** 3. idx_egress_id (Composite Index):
**    - Composite index on the columns 'dest_node', 'dest_service', 'egress_attempted', 'action_timestamp', and 'id'.
**    - This index optimizes queries that filter by node and service ranges, filter by egress_attempted (0),
**      and sort by action_timestamp. It can also enable an index-only scan to quickly retrieve 'id'.
**    - This composite index is designed for loading egress bundles by batch for a particular EgressID (A channel or contact)
**
** 4. idx_egress_attempted:
**    - Index on the 'egress_attempted' column in the 'bundle_data' table. This index is designed to speed up
**      DELETE queries and other queries filtering by 'egress_attempted'.
**
** 5. idx_bundle_id
**    - Index on the bplib-assigned unique 'bundle_id' in the 'bundle_data' table. This is used to detect duplicate bundles
**      in storage and by Custody Transfer to request the deletion or retransmission of custodial bundles. Whether or not
**      to allow duplicate bundles in storage is toggled by the BPLIB_ALLOW_DUPLICATE_BUNDLES flag.
**/

const char* CreateTableSQL =
"CREATE TABLE IF NOT EXISTS bundle_data (\n"
"    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
#if BPLIB_ALLOW_DUPLICATE_BUNDLES == false
"    bundle_id INTEGER UNIQUE,\n"
#else
"    bundle_id INTEGER,\n"
#endif
"    action_timestamp INTEGER,\n"
"    retransmit_timestamp INTEGER,\n"
"    retransmit_trigger INTEGER,\n"
"    egress_attempted INTEGER DEFAULT 0,\n"
"    dest_node INTEGER,\n"
"    dest_service INTEGER,\n"
"    is_custodial INTEGER,\n"
"    bundle_bytes INTEGER\n"
");\n"
"\n"
"CREATE TABLE IF NOT EXISTS bundle_blobs (\n"
"    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
"    bundle_row INTEGER,\n"
"    blob_data BLOB,\n"
"    FOREIGN KEY (bundle_row) REFERENCES bundle_data(id) ON DELETE CASCADE\n"
");\n"
"\n"
"CREATE TABLE IF NOT EXISTS bundle_recovery (\n"
"    bundle_row INTEGER PRIMARY KEY REFERENCES bundle_data(id) ON DELETE CASCADE,\n"
"    format INTEGER NOT NULL CHECK(format=2),\n"
"    ingress_id INTEGER NOT NULL CHECK(ingress_id BETWEEN 0 AND 4294967295),\n"
"    local_bundle INTEGER NOT NULL CHECK(local_bundle IN (0,1)),\n"
"    wire_bytes INTEGER NOT NULL CHECK(wire_bytes>0),\n"
"    receipt_time INTEGER NOT NULL CHECK(receipt_time>=0),\n"
"    receipt_era INTEGER NOT NULL CHECK(receipt_era BETWEEN 0 AND 4294967295)\n"
");\n"
"CREATE INDEX IF NOT EXISTS idx_bundle_blobs ON bundle_blobs (bundle_row);\n"
"CREATE INDEX IF NOT EXISTS idx_action_timestamp ON bundle_data (action_timestamp);\n"
#if BPLIB_ALLOW_DUPLICATE_BUNDLES == false
"CREATE UNIQUE INDEX IF NOT EXISTS idx_bundle_id ON bundle_data (bundle_id);\n"
#else
"CREATE INDEX IF NOT EXISTS idx_bundle_id ON bundle_data (bundle_id);\n"
#endif
"\n"
"CREATE INDEX IF NOT EXISTS idx_egress_id\n"
"ON bundle_data (\n"
"    dest_node,\n"
"    dest_service,\n"
"    egress_attempted,\n"
"    action_timestamp,\n"
"    is_custodial,\n"
"    id\n"
");\n"
"\n"
"CREATE INDEX IF NOT EXISTS idx_egress_attempted\n"
"ON bundle_data (egress_attempted);\n";


const char* GetNumBundlesSQL =
"SELECT COUNT(*) FROM bundle_data;";

const char* GetNumEgressedSQL =
"SELECT SUM(egress_attempted) "
"As NumEgressed "
"FROM bundle_data;";

const char* TotalBytesSQL =
"SELECT SUM(bundle_bytes) "
"AS TotalBytes "
"FROM bundle_data;";

/* Validated helper queries keep the exported raw SQL text compatible. */
static const char GetNumBundlesAccountingSQL[] =
"SELECT COUNT(*), 0 FROM bundle_data;";

static const char GetNumEgressedAccountingSQL[] =
"SELECT COUNT(CASE WHEN egress_attempted=1 THEN 1 END), "
"COUNT(CASE WHEN typeof(egress_attempted)!='integer' OR egress_attempted NOT IN (0,1) THEN 1 END) "
"FROM bundle_data;";

static const char TotalBytesAccountingSQL[] =
"SELECT COALESCE(SUM(bundle_bytes),0), "
"COUNT(CASE WHEN typeof(bundle_bytes)!='integer' OR bundle_bytes<0 THEN 1 END) "
"FROM bundle_data;";

const char* GetExpiredSQL = 
"SELECT id, bundle_bytes, bundle_id, is_custodial FROM bundle_data "
"WHERE (action_timestamp < ?) AND (egress_attempted = 0) "
"LIMIT ?;";

const char* DeleteBundleSQL =
"DELETE FROM bundle_data WHERE id = ?;";

const char* GetEgressedSQL = 
"SELECT id, bundle_bytes, bundle_id, is_custodial FROM bundle_data "
"WHERE egress_attempted = 1 LIMIT ?;";

/* ==================== */
/* Function Definitions */
/* ==================== */

SQL_Status_t BPLib_SQL_InitDb(const char* DbName, sqlite3** ActiveDbPtr)
{
    SQL_Status_t  SQLStatus;
    sqlite3*      ActiveDb;
    uint8_t       ForeignKeysEnabled;
    sqlite3_stmt* ForeignKeyCheckStmt;

    SQLStatus = sqlite3_open(DbName, ActiveDbPtr);
    if (SQLStatus != SQLITE_OK)
    {
        return SQLStatus;
    }

    ActiveDb = *ActiveDbPtr;

    SQLStatus = sqlite3_busy_timeout(ActiveDb, BPLIB_SQL_BUSY_TIMEOUT_MSEC);
    if (SQLStatus != SQLITE_OK)
    {
        return SQLStatus;
    }

    /* Set the atomic commit and rollback method to write-ahead log */
    SQLStatus = sqlite3_exec(ActiveDb, "PRAGMA journal_mode=WAL;", 0, 0, NULL);
    if (SQLStatus != SQLITE_OK)
    {
        return SQLStatus;
    }

    /* Enable foreign key support */
    SQLStatus = sqlite3_exec(ActiveDb, "PRAGMA foreign_keys=ON;", 0, 0, NULL);
    if (SQLStatus != SQLITE_OK)
    {
        return SQLStatus;
    }

    /* Custody acceptance follows commit: keep WAL commits durable. */
    SQLStatus = sqlite3_exec(ActiveDb, "PRAGMA synchronous=FULL;", 0, 0, NULL);
    if (SQLStatus != SQLITE_OK)
    {
        return SQLStatus;
    }

    /* Page size should already be 4096 by default, this just enforces it */
    SQLStatus = sqlite3_exec(ActiveDb, "PRAGMA page_size=4096;", 0, 0, NULL);
    if (SQLStatus != SQLITE_OK)
    {
        return SQLStatus;
    }

    /* Note: Apparently SQLite3 can have foreign_keys=ON fail SILENTLY if
    ** libsqlite3.so wasn't compiled with foreign key support. We have to manually
    ** check if foreign keys were enabled by reading the setting back.
    */
    ForeignKeysEnabled = 0;
    SQLStatus = sqlite3_prepare_v2(ActiveDb, "PRAGMA foreign_keys;", -1, &ForeignKeyCheckStmt, NULL);
    if (SQLStatus != SQLITE_OK)
    {
        return SQLStatus;
    }

    /* Check if the execution of foreign key enabling succeeded */
    if (sqlite3_step(ForeignKeyCheckStmt) == SQLITE_ROW)
    {
        ForeignKeysEnabled = sqlite3_column_int(ForeignKeyCheckStmt, 0);
    }

    sqlite3_finalize(ForeignKeyCheckStmt);
    if (ForeignKeysEnabled != 1)
    {
        fprintf(stderr, "Please use a SQLite3 compiled with Foreign Key Support.\n");
        return SQLITE_MISUSE;
    }

    return SQLITE_OK;
}

/** Create or update storage tables and read persisted accounting before publishing the initial
 * counters. */
SQL_Status_t BPLib_SQL_InitTable(BPLib_Instance_t* Inst)
{
    /* One SELECT observes a consistent accounting snapshot. Reject malformed
     * metadata rather than letting SUM coercion or unsigned conversion hide it. */
    const char *AccountingSQL =
        "SELECT COUNT(*), COUNT(CASE WHEN egress_attempted=1 THEN 1 END), "
        "COALESCE(SUM(bundle_bytes),0), "
        "COUNT(CASE WHEN typeof(bundle_bytes)!='integer' OR bundle_bytes<0 OR "
        "typeof(egress_attempted)!='integer' OR egress_attempted NOT IN (0,1) THEN 1 END) "
        "FROM bundle_data;";
    sqlite3_stmt *Accounting = NULL;
    sqlite3_int64 Values[4] = {0};
    SQL_Status_t Status = sqlite3_exec(Inst->BundleStorage.db, CreateTableSQL, 0, 0, NULL);
    if (Status != SQLITE_OK) return Status;
    Status = sqlite3_prepare_v2(Inst->BundleStorage.db, AccountingSQL, -1, &Accounting, NULL);
    if (Status == SQLITE_OK)
    {
        Status = sqlite3_step(Accounting);
        if (Status == SQLITE_ROW)
        {
            Status = SQLITE_OK;
            for (size_t i = 0; i < 4; ++i)
            {
                if (sqlite3_column_type(Accounting, i) != SQLITE_INTEGER ||
                    (Values[i] = sqlite3_column_int64(Accounting, i)) < 0)
                { Status = SQLITE_CORRUPT; break; }
            }
            if (Status == SQLITE_OK && (Values[3] != 0 || Values[1] > Values[0]))
                Status = SQLITE_CORRUPT;
            if (Status == SQLITE_OK && ((uint64_t)Values[0] > UINT32_MAX ||
                                       (uint64_t)Values[2] > SIZE_MAX))
                Status = SQLITE_TOOBIG;
            if (Status == SQLITE_OK)
            {
                Status = sqlite3_step(Accounting);
                Status = Status == SQLITE_DONE ? SQLITE_OK :
                         Status == SQLITE_ROW ? SQLITE_CORRUPT : Status;
            }
        }
        else if (Status == SQLITE_DONE) Status = SQLITE_CORRUPT;
    }
    int FinalStatus = sqlite3_finalize(Accounting);
    if (Status == SQLITE_OK) Status = FinalStatus;
    if (Status != SQLITE_OK) return Status;

    /* Publish together only after the read and cleanup have succeeded. */
    Inst->BundleStorage.BundleCountStored = (uint32_t)Values[0];
    Inst->BundleStorage.BundleCountNotEgressed = (size_t)(Values[0] - Values[1]);
    Inst->BundleStorage.BytesStorageInUse = (size_t)Values[2];
    return SQLITE_OK;
}

/** Open and initialize the instance’s SQLite store. Close and clear the handle if schema or
 * accounting recovery fails. */
BPLib_Status_t BPLib_SQL_Init(BPLib_Instance_t* Inst, const char* DbName)
{
    BPLib_Status_t Status;
    SQL_Status_t   SQLStatus;

    if (Inst == NULL || DbName == NULL) return BPLIB_NULL_PTR_ERROR;
    Status = BPLIB_SUCCESS;

    SQLStatus = BPLib_SQL_InitDb(DbName, &Inst->BundleStorage.db);
    if (SQLStatus == SQLITE_OK)
    {
        SQLStatus = BPLib_SQL_InitTable(Inst);
    }

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Storage SQL initialization failed (%d): %s\n", SQLStatus,
                Inst->BundleStorage.db ? sqlite3_errmsg(Inst->BundleStorage.db) : "no database handle");
        if (Inst->BundleStorage.db != NULL)
        {
            sqlite3_close(Inst->BundleStorage.db);
            Inst->BundleStorage.db = NULL;
        }
        Status = BPLIB_STOR_SQL_INIT_ERR;
    }

    return Status;
}

/* Query returns a value and the number of malformed input rows. Each public
 * helper preserves the caller's result unless validation and cleanup succeed. */
static int BPLib_SQL_ReadAccountingValue(sqlite3 *db, const char *Query,
                                       uint64_t Maximum, uint64_t *Result)
{
    if (db == NULL || Query == NULL || Result == NULL) return SQLITE_MISUSE;
    sqlite3_stmt *Statement = NULL;
    sqlite3_int64 Value = 0;
    int Status = sqlite3_prepare_v2(db, Query, -1, &Statement, NULL);
    if (Status == SQLITE_OK)
    {
        Status = sqlite3_step(Statement);
        if (Status == SQLITE_ROW)
        {
            if (sqlite3_column_count(Statement) != 2 ||
                sqlite3_column_type(Statement, 0) != SQLITE_INTEGER ||
                sqlite3_column_type(Statement, 1) != SQLITE_INTEGER ||
                (Value = sqlite3_column_int64(Statement, 0)) < 0 ||
                sqlite3_column_int64(Statement, 1) != 0)
                Status = SQLITE_CORRUPT;
            else if ((uint64_t)Value > Maximum) Status = SQLITE_TOOBIG;
            else
            {
                Status = sqlite3_step(Statement);
                Status = Status == SQLITE_DONE ? SQLITE_OK :
                         Status == SQLITE_ROW ? SQLITE_CORRUPT : Status;
            }
        }
        else if (Status == SQLITE_DONE) Status = SQLITE_CORRUPT;
    }
    int FinalStatus = sqlite3_finalize(Statement);
    if (Status == SQLITE_OK) Status = FinalStatus;
    if (Status == SQLITE_OK) *Result = (uint64_t)Value;
    else if (Status == SQLITE_CORRUPT || Status == SQLITE_TOOBIG)
        fprintf(stderr, "Invalid storage accounting value (%d)\n", Status);
    else fprintf(stderr, "Storage accounting query failed (%d): %s\n", Status, sqlite3_errmsg(db));
    return Status;
}

/** Read a validated total row count; write the caller’s output only on success. */
SQL_Status_t BPLib_SQL_GetNumStoredBundles(sqlite3 *db, uint32_t *BundleCnt)
{
    if (BundleCnt == NULL) return SQLITE_MISUSE;
    uint64_t Result = 0;
    int Status = BPLib_SQL_ReadAccountingValue(db, GetNumBundlesAccountingSQL, UINT32_MAX, &Result);
    if (Status == SQLITE_OK) *BundleCnt = (uint32_t)Result;
    return Status;
}

/** Read the count of rows marked attempted, rejecting invalid flag values without changing the
 * caller’s output. */
SQL_Status_t BPLib_SQL_GetNumEgressed(sqlite3 *db, size_t *EgressCnt)
{
    if (EgressCnt == NULL) return SQLITE_MISUSE;
    uint64_t Result = 0;
    int Status = BPLib_SQL_ReadAccountingValue(db, GetNumEgressedAccountingSQL, SIZE_MAX, &Result);
    if (Status == SQLITE_OK) *EgressCnt = (size_t)Result;
    return Status;
}

/** Read a validated sum of stored bundle bytes, treating an empty store as zero. */
SQL_Status_t BPLib_SQL_GetTotalBundleBytes(sqlite3 *db, uint64_t *TotalBytes)
{
    if (TotalBytes == NULL) return SQLITE_MISUSE;
    uint64_t Result = 0;
    int Status = BPLib_SQL_ReadAccountingValue(db, TotalBytesAccountingSQL, UINT64_MAX, &Result);
    if (Status == SQLITE_OK) *TotalBytes = Result;
    return Status;
}

/* Read one integer PRAGMA without publishing a partial result. */
static int BPLib_SQL_ReadSizePragma(sqlite3 *db, const char *Query, sqlite3_int64 *Value)
{
    sqlite3_stmt *Statement = NULL;
    int Status = sqlite3_prepare_v2(db, Query, -1, &Statement, NULL);
    sqlite3_int64 Result = 0;
    if (Status == SQLITE_OK)
    {
        Status = sqlite3_step(Statement);
        if (Status == SQLITE_ROW)
        {
            if (sqlite3_column_type(Statement, 0) != SQLITE_INTEGER ||
                (Result = sqlite3_column_int64(Statement, 0)) < 0)
                Status = SQLITE_CORRUPT;
            else
            {
                Status = sqlite3_step(Statement);
                Status = Status == SQLITE_DONE ? SQLITE_OK :
                         Status == SQLITE_ROW ? SQLITE_CORRUPT : Status;
            }
        }
        else if (Status == SQLITE_DONE) Status = SQLITE_CORRUPT;
    }
    int FinalStatus = sqlite3_finalize(Statement);
    if (Status == SQLITE_OK) Status = FinalStatus;
    if (Status == SQLITE_OK) *Value = Result;
    return Status;
}

/** Read SQLite page geometry and compute the database size in bytes with checked
 * multiplication. Leave the output unchanged on failure. */
BPLib_Status_t BPLib_SQL_GetDbSize(BPLib_Instance_t *Inst, size_t *DbSize)
{
    if (DbSize == NULL) return BPLIB_NULL_PTR_ERROR;
    *DbSize = 0;
    if (Inst == NULL) return BPLIB_NULL_PTR_ERROR;
    sqlite3 *db = Inst->BundleStorage.db;
    if (db == NULL) return BPLIB_ERROR;

    sqlite3_int64 PageCount = 0, PageSize = 0;
    if (BPLib_SQL_ReadSizePragma(db, "PRAGMA page_count;", &PageCount) != SQLITE_OK ||
        BPLib_SQL_ReadSizePragma(db, "PRAGMA page_size;", &PageSize) != SQLITE_OK ||
        PageSize <= 0 || (uint64_t)PageCount > SIZE_MAX / (uint64_t)PageSize)
        return BPLIB_ERROR;

    *DbSize = (size_t)((uint64_t)PageCount * (uint64_t)PageSize);
    return BPLIB_SUCCESS;
}

/* Collect first, then delete atomically. Custody and byte accounting follow
 * COMMIT so a failed DELETE cannot discard the in-memory custody entry. */
static SQL_Status_t BPLib_SQL_DiscardBatch(BPLib_Instance_t *Inst, size_t *NumDiscarded,
                                         sqlite3_stmt *Select, sqlite3_stmt *DeleteBundleStmt, bool Expired)
{
    sqlite3 *db = Inst->BundleStorage.db;
    SQL_Status_t Status;
    size_t Count = 0, Bytes = 0, CustodialCount = 0;
    uint32_t *CustodialIds = NULL;
    *NumDiscarded = 0;

    Status = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    if (Status != SQLITE_OK) return Status;
    sqlite3_reset(Select);
    int BindIndex = 1;
    if (Expired)
    {
        Status = sqlite3_bind_int64(Select, BindIndex++, BPLib_TIME_GetMonotonicTime());
        if (Status != SQLITE_OK) goto rollback;
    }
    Status = sqlite3_bind_int64(Select, BindIndex, BPLIB_STOR_DISCARDBATCHSIZE);
    if (Status != SQLITE_OK) goto rollback;
    while ((Status = sqlite3_step(Select)) == SQLITE_ROW)
    {
        sqlite3_int64 RowBytes = sqlite3_column_int64(Select, 1);
        if (Count >= BPLIB_STOR_DISCARDBATCHSIZE || RowBytes < 0 ||
            (uint64_t)RowBytes > SIZE_MAX - Bytes)
        {
            Status = SQLITE_CORRUPT;
            goto rollback;
        }
        Inst->BundleStorage.BundleRowsToDelete[Count++] = sqlite3_column_int64(Select, 0);
        Bytes += (size_t)RowBytes;
        if (sqlite3_column_int(Select, 3))
        {
            /* Keep the large configurable batch off the flight task's stack. */
            if (CustodialIds == NULL)
            {
                CustodialIds = malloc(BPLIB_STOR_DISCARDBATCHSIZE * sizeof(*CustodialIds));
                if (CustodialIds == NULL) { Status = SQLITE_NOMEM; goto rollback; }
            }
            CustodialIds[CustodialCount++] = (uint32_t)sqlite3_column_int64(Select, 2);
        }
    }
    if (Status != SQLITE_DONE) goto rollback;
    if (Bytes > Inst->BundleStorage.BytesStorageInUse)
    {
        Status = SQLITE_CORRUPT;
        goto rollback;
    }
    for (size_t Row = 0; Row < Count; ++Row)
    {
        sqlite3_reset(DeleteBundleStmt);
        Status = sqlite3_bind_int64(DeleteBundleStmt, 1, Inst->BundleStorage.BundleRowsToDelete[Row]);
        if (Status != SQLITE_OK) goto rollback;
        Status = sqlite3_step(DeleteBundleStmt);
        if (Status != SQLITE_DONE) goto rollback;
        if (sqlite3_changes(db) != 1) { Status = SQLITE_CONSTRAINT; goto rollback; }
    }
    Status = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if (Status != SQLITE_OK) goto rollback;

    *NumDiscarded = Count;
    Inst->BundleStorage.BytesStorageInUse -= Bytes;
    for (size_t Row = 0; Row < CustodialCount; ++Row)
        BPLib_CT_DeleteBundleFromCtdb(Inst, CustodialIds[Row]);
    free(CustodialIds);
    return SQLITE_OK;

rollback:
    /* Reset active statements and preserve the original failure over rollback. */
    sqlite3_reset(Select);
    sqlite3_reset(DeleteBundleStmt);
    if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) != SQLITE_OK)
        fprintf(stderr, "Failed to roll back discard batch: %s\n", sqlite3_errmsg(db));
    free(CustodialIds);
    return Status;
}

/* Own both statements across collection, deletion, rollback and finalization. */
static SQL_Status_t BPLib_SQL_DiscardOwned(BPLib_Instance_t *Inst, size_t *NumDiscarded, bool Expired)
{
    if (NumDiscarded) *NumDiscarded = 0;
    if (Inst == NULL || NumDiscarded == NULL) return SQLITE_MISUSE;
    sqlite3_stmt *Select = NULL, *DeleteBundleStmt = NULL;
    SQL_Status_t Status = sqlite3_prepare_v2(Inst->BundleStorage.db,
        Expired ? GetExpiredSQL : GetEgressedSQL, -1, &Select, NULL);
    if (Status == SQLITE_OK)
        Status = sqlite3_prepare_v2(Inst->BundleStorage.db, DeleteBundleSQL, -1, &DeleteBundleStmt, NULL);
    if (Status == SQLITE_OK)
        Status = BPLib_SQL_DiscardBatch(Inst, NumDiscarded, Select, DeleteBundleStmt, Expired);
    sqlite3_finalize(Select);
    sqlite3_finalize(DeleteBundleStmt);
    return Status;
}

/** Select the expired or egressed deletion path and manage its operation-local prepared
 * statements. */
static BPLib_Status_t BPLib_SQL_Discard(BPLib_Instance_t *Inst, size_t *NumDiscarded, bool Expired)
{
    if (NumDiscarded) *NumDiscarded = 0;
    if (Inst == NULL || NumDiscarded == NULL) return BPLIB_NULL_PTR_ERROR;
    return BPLib_SQL_DiscardOwned(Inst, NumDiscarded, Expired) == SQLITE_OK ?
        BPLIB_SUCCESS : BPLIB_STOR_SQL_DISCARD_ERR;
}

/** Delete eligible expired bundles and report the committed number removed. */
BPLib_Status_t BPLib_SQL_DiscardExpired(BPLib_Instance_t *Inst, size_t *NumDiscarded)
{
    return BPLib_SQL_Discard(Inst, NumDiscarded, true);
}

/** Run the expired selection and deletion using the caller’s statements and report only
 * committed work. */
SQL_Status_t BPLib_SQL_DiscardExpiredImpl(sqlite3 *db, size_t *NumDiscarded, BPLib_Instance_t *Inst)
{
    (void)db;
    return BPLib_SQL_DiscardOwned(Inst, NumDiscarded, true);
}

/** Delete rows already marked egressed and report the committed number removed. */
BPLib_Status_t BPLib_SQL_DiscardEgressed(BPLib_Instance_t *Inst, size_t *NumDiscarded)
{
    return BPLib_SQL_Discard(Inst, NumDiscarded, false);
}

/** Run the egressed selection and deletion using the caller’s statements and report only
 * committed work. */
SQL_Status_t BPLib_SQL_DiscardEgressedImpl(BPLib_Instance_t *Inst, size_t *NumDiscarded)
{
    return BPLib_SQL_DiscardOwned(Inst, NumDiscarded, false);
}

BPLib_Status_t BPLib_SQL_Cleanup(BPLib_Instance_t* Inst)
{
    BPLib_Status_t Status = BPLIB_SUCCESS;
    int SQLStatus;

    SQLStatus = sqlite3_exec(Inst->BundleStorage.db, "VACUUM;", 0, 0, NULL);

    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to vacuum: %s\n", sqlite3_errmsg(Inst->BundleStorage.db));
        Status = BPLIB_STOR_CLEANUP_ERR;
    }    

    return Status;
}

/** Build the supported EID range predicate in caller storage. Use bound parameters for values
 * and reject invalid ranges or a clause that will not fit. */
BPLib_Status_t BPLib_SQL_GetDestEidWhereClause(BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs, 
                                                                        char *WhereClause)
{
    size_t         MaxWhereLen;
    size_t         CurrWhereLen;
    size_t         i;
    const char*    FindForDestNodeSql_RangeClause = "((dest_node BETWEEN ? AND ?) AND";
    const char*    FindForDestNodeSql_EqualityClause = "((dest_node = ?) AND";
    const char*    FindForDestServSql_RangeClause = " (dest_service BETWEEN ? AND ?))";
    const char*    FindForDestServSql_EqalityClause = " (dest_service = ?))";
    const char**   FindClausePtr;

    if (DestEIDs == NULL || WhereClause == NULL) return BPLIB_NULL_PTR_ERROR;
    WhereClause[0] = '\0';
    if (NumEIDs == 0 || NumEIDs > BPLIB_MAX_CONTACT_DEST_EIDS) return BPLIB_STOR_PARAM_ERR;
    for (i = 0; i < NumEIDs; ++i)
    {
        if (DestEIDs[i].MaxNode == 0 && DestEIDs[i].MaxService == 0) break;
        if (DestEIDs[i].MinNode > DestEIDs[i].MaxNode || DestEIDs[i].MinService > DestEIDs[i].MaxService ||
            DestEIDs[i].MaxNode > INT64_MAX || DestEIDs[i].MaxService > INT64_MAX)
            return BPLIB_STOR_PARAM_ERR;
    }

    MaxWhereLen = BPLIB_SQL_MAX_STRLEN - 180; /* size of final query minus the non-where clause stuff */

    /* To keep search as efficient possible, we generate one combined query that contains all
    ** the DestEID patterns.
    **
    ** NOTE:
    ** This bit is tricky to understand from inspection. Basically, for each destination
    ** eid pattern node number or service number it checks if the pattern is a true pattern
    ** depicting a range, or if the minimum value equals the maximum value. In the former 
    ** case, it checks if either dest_node or dest_service is "BETWEEN ? AND ?". In the
    ** latter case it does an exact value check, so whether dest_node or dest_service "= ?".
    ** This is a minor optimization since exact queries are a bit faster in sqlite than 
    ** range queries. The final output should look something like 
    ** "((dest_node BETWEEN ? AND ?)) AND (dest_service = ?)) OR 
    **  ((dest_node = ?) AND (dest_service BETWEEN ? AND ?)) OR
    **  ((dest_node BETWEEN ? AND ?) AND (dest_service BETWEEN ? AND ?))",
    ** or any additional combinations of the sort, depending on the DestEIDs.
    */

    CurrWhereLen = 0;

    for (i = 0; i < NumEIDs; i++)
    {
        if (DestEIDs[i].MaxNode == 0 && DestEIDs[i].MaxService == 0)
        {
            /* dtn:none detected, skip this */
            break;
        }
        /* If maxNode == minNode, do an exact query, otherwise do a range query */
        if (DestEIDs[i].MaxNode == DestEIDs[i].MinNode)
        {
            CurrWhereLen += strlen(FindForDestNodeSql_EqualityClause);
            FindClausePtr = &FindForDestNodeSql_EqualityClause;
        }
        else
        {
            CurrWhereLen += strlen(FindForDestNodeSql_RangeClause);
            FindClausePtr = &FindForDestNodeSql_RangeClause;
        }

        if (CurrWhereLen >= MaxWhereLen)
        {
            fprintf(stderr, "Programming Error: Node WHERE clause too long\n");
            return BPLIB_STOR_SQL_OVERFLOW_ERR;       
        }
        else
        {
            /* Add the node query to the where clause */
            strncat(WhereClause, *FindClausePtr, MaxWhereLen - strlen(WhereClause));
        }

        /* If MaxService == MinService, do an exact query, otherwise do a range query */
        if (DestEIDs[i].MaxService == DestEIDs[i].MinService)
        {
            CurrWhereLen += strlen(FindForDestServSql_EqalityClause);
            FindClausePtr = &FindForDestServSql_EqalityClause;
        }
        else
        {
            CurrWhereLen += strlen(FindForDestServSql_RangeClause);
            FindClausePtr = &FindForDestServSql_RangeClause;
        }

        if (CurrWhereLen >= MaxWhereLen)
        {
            fprintf(stderr, "Programming Error: Node WHERE clause too long\n");
            return BPLIB_STOR_SQL_OVERFLOW_ERR;
        }
        else
        {
            /* Add the service query to the where clause */
            strncat(WhereClause, *FindClausePtr, MaxWhereLen - strlen(WhereClause));
        }

        /* Link multiple EID queries with an OR, unless this is the last EID */
        if (i != (NumEIDs - 1) && !(DestEIDs[i + 1].MaxNode == 0 && DestEIDs[i + 1].MaxService == 0))
        {
            if (CurrWhereLen + strlen(" OR ") >= MaxWhereLen)
            {
                fprintf(stderr, "Programming Error: OR WHERE clause too long\n");
                return BPLIB_STOR_SQL_OVERFLOW_ERR;
            }
            else
            {
                strncat(WhereClause, " OR ", MaxWhereLen - CurrWhereLen);
                CurrWhereLen += strlen(" OR ");
            }
        }
    }

    WhereClause[strlen(WhereClause)] = '\0';  
    
    return BPLIB_SUCCESS;
}
