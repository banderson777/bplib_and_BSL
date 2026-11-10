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

#include "bplib_stor_sql_cust.h"
#include "bplib_inst.h"
#include <stdio.h>

const char* SetNewRetransmitTriggerBaseSQL =
"WITH to_update AS (\n"
    "SELECT id FROM bundle_data INDEXED BY idx_egress_id WHERE (%s) \n"
    "AND is_custodial = 1 AND egress_attempted = 0\n"
")\n"
"UPDATE bundle_data SET retransmit_trigger = ?, retransmit_timestamp = ? \n"
"WHERE id IN (SELECT id FROM to_update);";

const char* TriggerRetransmitSQL =
"UPDATE bundle_data SET retransmit_timestamp = ? WHERE (bundle_id = ? AND egress_attempted = 0);";

const char* StopRetransmitSQL =
"UPDATE bundle_data SET retransmit_trigger = ? WHERE (bundle_id = ? AND egress_attempted = 0);";

const char* MarkForDeletionSQL =
"UPDATE bundle_data SET egress_attempted = 1 WHERE (bundle_id = ? AND egress_attempted = 0);";



/** Validate the destination filters and prepare a scoped update for unegressed custodial
 * bundles. Report the number updated only after commit. */
BPLib_Status_t BPLib_SQL_SetNewRetransmitTrigger(BPLib_Instance_t *Inst, uint32_t ContactId,
                BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs, size_t RetransmitTrigger, size_t *NumUpdated)
{
    BPLib_Status_t Status;
    SQL_Status_t   SQLStatus;
    sqlite3*       db;
    char           WhereClause[BPLIB_SQL_MAX_STRLEN] = {0};
    sqlite3_stmt  *SetRetransmitAllStmt = NULL;

    (void)ContactId;
    if (NumUpdated == NULL) return BPLIB_NULL_PTR_ERROR;
    *NumUpdated = 0;
    if (Inst == NULL || DestEIDs == NULL) return BPLIB_NULL_PTR_ERROR;
    if (NumEIDs == 0 || NumEIDs > BPLIB_MAX_CONTACT_DEST_EIDS || RetransmitTrigger > UINT32_MAX)
        return BPLIB_STOR_PARAM_ERR;
    db = Inst->BundleStorage.db;

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
    char *SetNewRetransmitTriggerSQL = sqlite3_mprintf(SetNewRetransmitTriggerBaseSQL, WhereClause);
    if (SetNewRetransmitTriggerSQL == NULL) return BPLIB_STOR_SQL_NEW_RETRANSMIT_ERR;

    /* Prepare Search Statements needed for this batch query */
    SQLStatus = sqlite3_prepare_v2(db, SetNewRetransmitTriggerSQL, -1, &SetRetransmitAllStmt, 0);
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Programming Error: SetNewRetransmitTriggerSQL prepare failed, error=%s\n", sqlite3_errmsg(db));
        Status = BPLIB_STOR_SQL_NEW_RETRANSMIT_ERR;
    }

    if (Status == BPLIB_SUCCESS)
    {
        /* Run Batch Load Logic */
        SQLStatus = BPLib_SQL_SetNewRetransmitTriggerImpl(db, DestEIDs, NumEIDs, 
                                                            RetransmitTrigger, NumUpdated, SetRetransmitAllStmt);

        if (SQLStatus != SQLITE_OK)
        {
            Status = BPLIB_STOR_SQL_NEW_RETRANSMIT_ERR;
        }
    }

    /* Cleanup/Finalize */
    sqlite3_finalize(SetRetransmitAllStmt);
    sqlite3_free(SetNewRetransmitTriggerSQL);
    
    return Status;
}

/** Bind filters and retransmission settings to the caller-owned statement, applying the update
 * transactionally. */
SQL_Status_t BPLib_SQL_SetNewRetransmitTriggerImpl(sqlite3* db, BPLib_EID_Pattern_t* DestEIDs, 
                            size_t NumEIDs, size_t RetransmitTrigger, size_t *NumUpdated, sqlite3_stmt *SetRetransmitAllStmt)
{
    SQL_Status_t  SQLStatus;
    size_t        i;
    uint64_t      BindIndex;

    *NumUpdated = 0;
    SQLStatus = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    if (SQLStatus != SQLITE_OK) return SQLStatus;

    /* Bind parameters for query */
    sqlite3_reset(SetRetransmitAllStmt);

    BindIndex = 1;
    /* Bind destination EID query */
    for (i = 0; i < NumEIDs; i++)
    {
        if (DestEIDs[i].MaxNode == 0 && DestEIDs[i].MaxService == 0)
        {
            break;
        }

        SQLStatus = sqlite3_bind_int64(SetRetransmitAllStmt, BindIndex++, DestEIDs[i].MinNode);
        if (SQLStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to bind dest_node min: %s\n", sqlite3_errmsg(db));
            goto rollback;
        }

        if (DestEIDs[i].MinNode != DestEIDs[i].MaxNode)
        {
            SQLStatus = sqlite3_bind_int64(SetRetransmitAllStmt, BindIndex++, DestEIDs[i].MaxNode);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "Failed to bind dest_node max: %s\n", sqlite3_errmsg(db));
                goto rollback;
            }
        }

        SQLStatus = sqlite3_bind_int64(SetRetransmitAllStmt, BindIndex++, DestEIDs[i].MinService);
        if (SQLStatus != SQLITE_OK)
        {
            fprintf(stderr, "Failed to bind dest_node min: %s\n", sqlite3_errmsg(db));
            goto rollback;
        }

        if (DestEIDs[i].MinService != DestEIDs[i].MaxService)
        {
            SQLStatus = sqlite3_bind_int64(SetRetransmitAllStmt, BindIndex++, DestEIDs[i].MaxService);
            if (SQLStatus != SQLITE_OK)
            {
                fprintf(stderr, "Failed to bind dest_node max: %s\n", sqlite3_errmsg(db));
                goto rollback;
            }
        }

    }

    /* Bind retransmit_trigger to new value */
    SQLStatus = sqlite3_bind_int64(SetRetransmitAllStmt, BindIndex++, RetransmitTrigger);
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind retransmit_trigger: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }

    /* Bind retransmit_time to automatically trigger retransmission */
    SQLStatus = sqlite3_bind_int64(SetRetransmitAllStmt, BindIndex++, 
                                            BPLib_TIME_GetMonotonicTime());
    if (SQLStatus != SQLITE_OK)
    {
        fprintf(stderr, "Failed to bind retransmit_time: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }

    SQLStatus = sqlite3_step(SetRetransmitAllStmt);

    if (SQLStatus != SQLITE_DONE) goto rollback;
    size_t Updated = sqlite3_changes(db);
    SQLStatus = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if (SQLStatus != SQLITE_OK) goto rollback;
    *NumUpdated = Updated;
    return SQLITE_OK;
rollback:
    sqlite3_reset(SetRetransmitAllStmt);
    if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) != SQLITE_OK)
        fprintf(stderr, "Failed to roll back retransmit scheduling: %s\n", sqlite3_errmsg(db));
    return SQLStatus;
}

/** Prepare the start, stop, and delete updates for one batch. Finalize every statement and
 * adjust the unegressed count only on success. */
BPLib_Status_t BPLib_SQL_UpdateCustodialBundles(BPLib_Instance_t *Inst, BPLib_STOR_CtUpdateBatch_t *Batch)
{
    if (Inst == NULL || Batch == NULL) return BPLIB_NULL_PTR_ERROR;
    if (Batch->Size > BPLIB_STOR_CT_BATCH_SIZE) return BPLIB_STOR_PARAM_ERR;
    if (Batch->Size == 0) return BPLIB_SUCCESS;
    sqlite3 *db = Inst->BundleStorage.db;
    size_t NumEgressed = 0;
    BPLib_SQL_CustodyStatements_t Statements = {0};
    SQL_Status_t Status = sqlite3_prepare_v2(db, TriggerRetransmitSQL, -1, &Statements.Start, NULL);
    if (Status == SQLITE_OK)
        Status = sqlite3_prepare_v2(db, StopRetransmitSQL, -1, &Statements.Stop, NULL);
    if (Status == SQLITE_OK)
        Status = sqlite3_prepare_v2(db, MarkForDeletionSQL, -1, &Statements.Delete, NULL);
    if (Status == SQLITE_OK)
        Status = BPLib_SQL_UpdateCustodialBundlesImpl(db, Batch, &NumEgressed, &Statements);
    sqlite3_finalize(Statements.Delete);
    sqlite3_finalize(Statements.Stop);
    sqlite3_finalize(Statements.Start);
    if (Status != SQLITE_OK) return BPLIB_SQL_CUSTODY_UPDATE_ERR;
    Inst->BundleStorage.BundleCountNotEgressed -= NumEgressed;
    return BPLIB_SUCCESS;
}

/** Execute the batch in one transaction. Roll back on invalid operations or SQL errors and
 * report no committed deletes on failure. */
SQL_Status_t BPLib_SQL_UpdateCustodialBundlesImpl(sqlite3 *db, BPLib_STOR_CtUpdateBatch_t *Batch, size_t *NumEgressed, BPLib_SQL_CustodyStatements_t *Statements)
{
    *NumEgressed = 0;
    if (Batch->Size == 0) return SQLITE_OK;
    SQL_Status_t Status = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    if (Status != SQLITE_OK) return Status;
    for (size_t Row = 0; Row < Batch->Size; ++Row)
    {
        sqlite3_stmt *Statement;
        switch (Batch->Ops[Row])
        {
            case BPLIB_CT_MARK_DELETE: Statement = Statements->Delete; break;
            case BPLIB_CT_START_RETRANSMIT: Statement = Statements->Start; break;
            case BPLIB_CT_STOP_RETRANSMIT: Statement = Statements->Stop; break;
            default: Status = SQLITE_MISUSE; goto rollback;
        }
        sqlite3_reset(Statement);
        int BindIndex = 1;
        if (Batch->Ops[Row] != BPLIB_CT_MARK_DELETE)
        {
            Status = sqlite3_bind_int64(Statement, BindIndex++,
                Batch->Ops[Row] == BPLIB_CT_START_RETRANSMIT ? BPLib_TIME_GetMonotonicTime() : BPLIB_NO_RETRANSMIT_TRIGGER);
            if (Status != SQLITE_OK) goto rollback;
        }
        Status = sqlite3_bind_int64(Statement, BindIndex, Batch->BundleIDs[Row]);
        if (Status != SQLITE_OK) goto rollback;
        Status = sqlite3_step(Statement);
        if (Status != SQLITE_DONE) goto rollback;
        if (Batch->Ops[Row] == BPLIB_CT_MARK_DELETE) *NumEgressed += sqlite3_changes(db);
    }
    Status = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if (Status != SQLITE_OK) goto rollback;
    return SQLITE_OK;
rollback:
    sqlite3_reset(Statements->Delete);
    sqlite3_reset(Statements->Stop);
    sqlite3_reset(Statements->Start);
    if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) != SQLITE_OK)
        fprintf(stderr, "Failed to roll back custody batch: %s\n", sqlite3_errmsg(db));
    *NumEgressed = 0;
    return Status;
}
